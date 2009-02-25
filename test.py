""" This is a test of the ability to read an image.

You should have a image created beforehand using for example:

python facquire.py -S10 -e ntfs1-gen2.dd test

This test requires the sk python bindings and pyewf bindings because
we test access speed to each other.
"""

import sk, fif, pyewf, pyaff
import time, sys, glob

def test_time(fd):
    fs = sk.skfs(fd)
    f = fs.open('/Compressed/logfile1.txt')

    count =0 
    while 1:
        data=f.read(1024*1024*30)
        if len(data)==0: break

        count+=len(data)

    return count

def main():
    if 0:
        t=time.time()
        fd = open("/var/tmp/uploads/testimages/ntfs/ntfs1-gen2.dd")
        count=test_time(fd)
        print "RAW Read %s in %s" % (count, time.time()-t)


    if 1:
        t = time.time()
        ## Open all the volumes at once:
        fiffile = fif.FIFFile("test.00.zip")
        
        fd = fiffile.open_stream("data")
        count = test_time(fd)
        fiffile.stats()
        print "FIF Read %s in %s" % (count, time.time()-t)

    if 1:
        t = time.time()
        fd = pyewf.open(["test.E01",])
        count = test_time(fd)
        fd.close()
        print "EWF Read %s in %s" % (count, time.time()-t)

main()
sys.exit(0)

import hotshot
prof = hotshot.Profile("hotshot_edi_stats")
prof.runcall(main)
prof.close()

