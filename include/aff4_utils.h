/*
** aff4_utils.h
** 
** Made by mic
** Login   <mic@laptop>
** 
** Started on  Thu Nov 12 20:43:54 2009 mic
** Last update Wed Jan 27 11:02:12 2010 mic
*/

#ifndef   	AFF4_UTILS_H_
# define   	AFF4_UTILS_H_

#include "class.h"
#include "tdb.h"
#include "list.h"

/* This is a function that will be run when the library is imported. It
   should be used to initialise static stuff.

   It will only be called once. If you require some other subsystem to
   be called first you may call it from here.
*/
#define AFF4_MODULE_INIT(name) static int name ## _initialised = 0;     \
  static inline void name ## _init_2();                                 \
  void name ## _init() {                                                \
    if(name ## _initialised) return;                                    \
    name ## _initialised = 1;                                           \
    name ## _init_2();                                                  \
  };                                                                    \
  inline void name ## _init_2()

#define HASH_TABLE_SIZE 256
#define CACHE_SIZE 15

/** A cache is an object which automatically expires data which is
    least used - that is the data which is most used is put at the end
    of the list, and when memory pressure increases we expire data
    from the front of the list.
*/
PRIVATE enum Cache_policy {
  CACHE_EXPIRE_FIRST,
  CACHE_EXPIRE_LEAST_USED
};

/** The Cache is an object which manages a cache of Object instances
    indexed by a key.

    Each Object can either be in the Cache (in which case it is owned
    by this cache object and will not be freed) or out of the cache
    (in which case it is owned by NULL, and can be freed. When the
    cache gets too full it will start freeing objects it owns.

    Objects are cached with a key reference so this is like a python
    dict or a hash table, except that we can have several objects
    indexed with the same key. If you want to iterate over all the
    objects you need to use the iterator interface.

    NOTE: The cache must be locked if you are using it from multiple
    threads. You can use the Cache->lock mutex or some other mutex
    instead.

    NOTE: After putting the Object in the cache you do not own it -
    and you must not use it (because it might be freed at any time).
*/
PRIVATE CLASS(Cache, Object)
/* The key which is used to access the data */
     char *key;
     int key_len;

     /* An opaque data object and its length. The object will be
        talloc_stealed into the cache object as we will be manging its
        memory.
     */
     Object data;

     /* Cache objects are put into two lists - the cache_list contains
        all the cache objects currently managed by us in order of
        least used to most used at the tail of the list. The same
        objects are also present on one of the hash lists which hang
        off the respective hash table. The hash_list should be shorter
        to search linearly as it only contains objects with the same
        hash.
     */
     struct list_head cache_list;
     struct list_head hash_list;

     /* This is a pointer to the head of the cache */
     struct Cache_t *cache_head;
     enum Cache_policy policy;

     /* The current number of objects managed by this cache */
     int cache_size;

     /* The maximum number of objects which should be managed */
     int max_cache_size;

     /* A hash table of the keys */
     int hash_table_width;
     Cache *hash_table;

     /* These functions can be tuned to manage the hash table. The
        default implementation assumes key is a null terminated
        string.
     */
     int METHOD(Cache, hash, char *key, int len);
     int METHOD(Cache, cmp, char *other, int len);

     /** hash_table_width is the width of the hash table.
         if max_cache_size is 0, we do not expire items.

         DEFAULT(hash_table_width) = 100;
         DEFAULT(max_cache_size) = 0;
     */
     Cache METHOD(Cache, Con, int hash_table_width, int max_cache_size);

     /* Return a cache object or NULL if its not there. The
        object is removed from the cache.
     */
     Object METHOD(Cache, get, char *key, int len);

     /* Returns a reference to the object. The object is still owned
      by the cache. Note that this should only be used in caches which
      do not expire objects or if the cache is locked, otherwise the
      borrowed reference may disappear unexpectadly. References may be
      freed when other items are added.
     */
     BORROWED Object METHOD(Cache, borrow, char *key, int len);

     /* Store the key, data in a new Cache object. The key and data will be
        stolen.
     */
     BORROWED Cache METHOD(Cache, put, char *key, int len, Object data);

     /* Returns true if the object is in cache */
     int METHOD(Cache, present, char *key, int len);

     /* This returns an opaque reference to a cache iterator. Note:
        The cache must be locked the entire time between receiving the
        iterator and getting all the objects.
     */
     BORROWED Object METHOD(Cache, iter, char *key, int len);
     BORROWED Object METHOD(Cache, next, Object *iter);

     int METHOD(Cache, print_cache);
END_CLASS


struct RDFURN_t;
     /** A logger may be registered with the Resolver. Any objects
         obtained from the Resolver will then use the logger to send
         messages to the user.
     */
CLASS(Logger, Object)
     Logger METHOD(Logger, Con);
     /* The message sent to the logger is sent from a service (The
     source of the message), at a particular level (how critical it
     is). It talks about a subject (usually the URN of the subject),
     and a message about it.
     */
     void METHOD(Logger, message, int level,\
                 char *service, struct RDFURN_t *subject, char *message);
END_CLASS

PROXY_CLASS(Logger);

#endif 	    /* !AFF4_UTILS_H_ */
