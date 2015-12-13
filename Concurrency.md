# Introduction #

AFF4 is a unique format because it encourages multiple access to the evidence file, for reading as well as for writing. There are multiple streams in the same file and therefore multiple threads need to be able to create them.

Concurrency is therefore extremely important - not just for thread synchronization, but also synchronizing processes accessing the same evidence container is important.

This document explains the rationale behind the current concurrency model and its design.

# Overview #

We wanted AFF4 to be as simple as possible for use. It has automatically generated python bindings making it easy for use from scripting languages, as well as from C, C++ or Java. This means we did not want users to have to worry about concurrency and synchronization, the library should take care of that behind the scenes. The following code samples are in Python for simplicity, but the equivalent C code is the same.

AFF4 is designed around the concept of a central resolver. The resolver mediates access to all the AFF4 objects and is a central point for managing all the metadata and attributes for all the objects. The resolver is a singleton object which may be created in multiple threads or processes and accesses the same underlying data storage and synchronization engine. This makes it possible to synchronize multiple threads and processes easily by shifting the complexity of interprocess and interthread communication away from the application and into the library.

The resolver has a number of different interfaces to provide different services:

### Metadata services ###

The resolver stores information tuples in RDF format. This includes setting, adding and resolving attributes for URNs.

### Object Management ###

Managing AFF4 Objects through their life cycle of creation, loading and closing. Details of the specific protocol are given below. An overview includes:

  * creating new objects
  * loading existing volumes
  * managing object caches
    * Objects are cached for rapid access. This allows objects to be conservatively created, and synchronization possible between multiple users of the same object. For example consider a FileBackedObject which provides read access to [file:///foobar.aff4](file:///foobar.aff4). Multiple threads might want to read from the file at the same time. Rather than open multiple readers, the Resolver manages a single reader in the cache and this reader is shared by different users as needed.
    * There are two caches:
      * a read cache is purely a performance enhancement so we dont need to instantiate too many readers. It is possible to have multiple readers with the same URN in the cache at the same time.
      * The write cache is exclusive - that means that if a thread has an object opened for writing, it will be locked and all subsequent accesses to the object will block. Write synchronization is done through this exclusive write cache.

  * Thread synchronization.
    * It is important to ensure that threads do not access the same object in an unorderly fashion. For example consider a ZipFile object for a zip volume opened for writing. Multiple threads might need to grab the same object so they can add different segments at different times. It will be very bad if multiple threads were able to create two instances of the same object, or tried adding segments at the same time. The underlying file will get mixed up and corrupted.

# Usage patterns #

The following are common usage patterns. We describe how synchronization is achieved in each of these cases.

## Object Creation ##

When creating an AFF4 object there are many variations that can be made for each object, and each object requires different parameters. Rather than having different constructors for each object type, the API maintains consistency and flexibility by following a very simple pattern:

```
    # First we create an RDFURN to hold the reference 
    # to the location we want to store the volume on:
    stored = RDFURN()
    stored.set("file:///foobar.zip")

    ## Now a new zip file is created
    zip = oracle.create(AFF4_ZIP_VOLUME)

    ## The object is not complete, we need to set 
    ## properties such as where it should be stored. 
    ## NOTE: Attributes set on objects are not simply 
    ## strings, they are full instances of the required type. 
    ## In this case we need to store to a URN which means we 
    ## need to use a proper RDFURN object (A string will not work
    ## - this function will refuse to operate on a simple string 
    ## as a value and raise an exception if you try to pass it).
    zip.set(AFF4_STORED, stored)

    ## This completes the object - note that a new 
    ## object is returned (different from before). 
    ## This operation may raise an exception if any 
    ## of the parameters dont work out (e.g. cant open 
    ## underlying storage file).
    zip = zip.finish()

    ## Now do stuff with the object (e.g. add segments etc).

    ## Once we are done with the object, we return it to the cache.
    ## This now allows it to be used by other threads.
    oracle.cache_return(zip)
```

There is no synchronization provided in this protocol. The created object does not appear in the cache until it is returned with cache\_return(). Therefore it is important that other threads do not attempt to recreate the same object at the same time.

## Obtaining a handle to an existing Object ##

Once the object is created, we can obtain an instance of it at any time. If the object is in cache we can obtain it from the cache otherwise the object will be created and added to the cache.

  * Objects opened for reading:
    1. If the object is in cache
      * If the object is not locked, lock it.
      * If it is locked (in use by another thread) create a new object, add to the cache and lock it.
    1. Object is returned to the caller.
  * Objects opened for writing
    1. If the object is in cache:
      * Attempt to lock it - this will block on its mutex
      * Return the object

In both cases the end result for calling the **open()** function is that the object is in its respective cache and it's mutex is locked.

```
    urn = RDFURN()
    urn.set("aff4://123242-121-aa")

    ## This will raise if an error occurred opening this URI
    zip = oracle.open(urn, 'w')

    ## Do stuff with the zip file (e.g. write segments)

    ## Return the object to the cache
    oracle.cache_return(zip)
```

So between the open() and cache\_return() call, the object specified by its URN is locked. Another thread which tries to obtain that same object will block until the owning thread makes the cache\_return() call. NOTE that once a thread returns the object to the cache they no longer own it and must not touch it in any way.

## Closing off an object ##
When we are finished with an object we should call its close() method. This allows the object the opportunity to cleanup - for example, by calling the ZipFile.close() method a central directory and RDF serialization is written.

One of the issues behind calling close() is that while thread 1 might call close() on the object, thread 2 might be blocked on the same object. If the object becomes invalid after thread 1 calls close() then thread 2 is deadlocked without an opportunity to work with the object itself.

Consider the following scenario:

  * Thread 1
    1. open aff4 volume for writing
    1. create an image stream within the volume
    1. write on the image stream:
      * this will gradually fill a bevy
      * Once the bevy is full the aff4 volume is opened for writing (and locked)
      * The bevy is dumped into the volume as a new segment. This marks the volume as dirty - meaning the Central directory is invalid now.
    1. Once the image is finished, close the aff4 volume:
      1. Open the aff4 volume for writing (and lock it)
      1. This creates an AFF4 serialization (information.turtle)
      1. Writes a Central Directory and marks the volume as clean.
      1. Return the volume to the write cache
    1. Thread 1 terminates.

  * Thread 2 is doing the same thing as thread 1, but takes a bit longer.
    1. When thread 1 closes the volume in step 4 above, Thread 2 is trying to dump a bevy
      1. open the aff4 volume for writing
      1. Now thread 2 is suspended until thread 1 completes writing the central directory and marks the volume as clean.
      1. When thread 1 returns the volume object to the cache, thread 2 starts running
      1. A new bevy is dumped on the volume this marks the volume as dirty again, and actually overwrites the central directory just written by thread 1.
      1. The now dirty volume is returned to the cache
    1. Once thread 2 is finished it calls close on the volume
      1. Again, the updated central directory is written and the volume is marked clean.


In code this looks like:

```
    urn = RDFURN()
    urn.set("file:///foobar.aff4")

    zip = oracle.create(AFF4_ZIP_VOLUME)
    zip.set(AFF4_STORED, urn)

    zip = zip.finish()

    zip_urn = zip.urn
    ## We need to release the volume now - other threads may use it.
    oracle.cache_return(zip)

    ## Notice how we can no longer use the zip object - it is 
    ## now owned by the cache and it is unsafe for us to use it.
    stream = oracle.create(AFF4_IMAGE)
    stream.set(AFF4_STORED, zip_urn)

    stream = stream.finish()

    ## Copy data from fd into the new image stream
    while 1:
       data = fd.read(1024)
       if not data: break

       stream.write(data)

    ## First close the stream, then the volume
    stream.close()
    zip.close()
```



# Implementation Notes #
When implementing new AFF4 objects here are some points to remember:

  * When implementing any method of an AFF4 object it is assumed that the object itself is locked and can only be used by a single thread. This means that you dont need to worry about protecting your own private object parameters with mutexes. This happens because the only way a thread can obtain the object is to call resolver.open() on it which will automatically lock it for exclusive access, guaranteeing that no other thread is using it at the same time.
  * By the same token you can not assume that your object is aways being called from the same thread since one thread can return the object to the cache to be picked up by another thread. This means that persistent parameters should not be stored in the object itself, but must be re-obtained from the resolver in each function based on the URI of the object.
  * Since the object's thread context can change after it gets returned to the cache, objects should only be returned once their context is irrelevant.

For example, the Image implementation needs to write a bevy which contains a concatenated list of chunks. There are several possibilities:

  1. Create a new segment when a bevy is started and keep it around, writing small increments.
    * This is a bad idea since the bevy may take a long time to fill, and the aff4 volume is locked for the duration (until the segment is closed). This could lead to deadlocks when several threads are trying to update the same volume and depend on one another.
  1. Call open() on the segment, write a bit of data and then return it to the cache.
    * This is not going to work because while the segment is dirty (i.e. between the create and the close()) it holds the zip volume locked. This is because it is impossible to interleave segments as they are the smallest continuous unit.
  1. Buffer the data for the bevy in memory and when it is complete, open the segment, dump it in its entirety and call close() on it.
    * This is the best way since we do not need to wait long for the data to become available. We minimise the length of time the volume is locked and avoid possible deadlocks since we do not depend on further input to complete the bevy.