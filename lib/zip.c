/** This file implements the basic Zip handling code. We allow
    concurrent read/write.

    FIXME: The FileBackedObject needs to be tailored for windows.
*/
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <time.h>
#include <libgen.h>
#include "aff4.h"
#include "zip.h"

/** Implementation of FileBackedObject.

FileBackedObject is a FileLikeObject which uses a real file to back
itself.
*/
/** Note that files we create will always be escaped using standard
    URN encoding.
*/
static FileBackedObject FileBackedObject_Con(FileBackedObject self, 
					     char *urn, char mode) {
  int flags;
  char *buffer;
  char *filename;

  if(!startswith(urn, "file://")) {
    RaiseError(ERuntimeError,"You must have a fully qualified urn here, not '%s'", urn);
    return NULL;
  };

  filename = talloc_strdup(self, urn + strlen("file://"));

  // Set our urn - always fqn
  URNOF(self) = talloc_strdup(self, urn);

  if(mode == 'r') {
    flags = O_RDONLY | O_BINARY;
  } else {
    flags = O_CREAT | O_RDWR | O_BINARY;
    buffer = escape_filename(filename, filename);
    // Try to create leading directories
    _mkdir(dirname(buffer));
  };

  // Now try to create the file within the new directory
  buffer = escape_filename(filename,filename);
  self->fd = open(buffer, flags, S_IRWXU | S_IRWXG | S_IRWXO);
  if(self->fd<0){
    RaiseError(EIOError, "Can't open %s (%s)", urn, strerror(errno));
    goto error;
  };

  // Work out what the file size is:
  self->super.size = lseek(self->fd, 0, SEEK_END);
  // Check that its what we expected
  {
    uint64_t result=self->super.size;
    
    CALL(oracle, resolve2,URNOF(self), AFF4_SIZE,
	 AS_BUFFER(result),
	 RESOLVER_DATA_UINT64);

    if(self->super.size != result) {
      // The size is not what we expect. Therefore the data stored in
      // the resolver for this file is incorrect - we need to clear it
      // all.
      CALL(oracle, del, URNOF(self), NULL);
    };
  };
  
  CALL(oracle, set, URNOF(self), AFF4_SIZE, &self->super.size, RESOLVER_DATA_UINT64);
  talloc_free(filename);
  return self;

 error:
  talloc_free(filename);
  talloc_free(self);
  return NULL;
};

// This is the low level constructor for FileBackedObject.
static AFFObject FileBackedObject_AFFObject_Con(AFFObject self, char *urn, char mode) {
  FileBackedObject this = (FileBackedObject)self;

  self->mode = mode;

  if(urn) {
    self = (AFFObject)this->Con(this, urn, mode);
  } else {
    self = this->__super__->super.Con((AFFObject)this, urn, mode);
  };

  return self;
};

static int FileLikeObject_truncate(FileLikeObject self, uint64_t offset) {
  self->size = offset;
  self->readptr = min(self->size, self->readptr);
  return offset;
};

static uint64_t FileLikeObject_seek(FileLikeObject self, int64_t offset, int whence) {
  if(whence==SEEK_SET) {
    self->readptr = offset;
  } else if(whence==SEEK_CUR) {
    self->readptr += offset;
    if(self->readptr<0) self->readptr=0;
  } else if(whence==SEEK_END) {
    self->readptr = self->size + offset;
  };

  if(self->readptr < 0) {
    self->readptr=0;

  } else if(self->readptr > self->size) {
    self->readptr = self->size;
  };

  return self->readptr;
};
  
/** 
    read some data from our file into the buffer (which is assumed to
    be large enough).
**/
static int FileBackedObject_read(FileLikeObject self, char *buffer, unsigned long int length) {
  FileBackedObject this = (FileBackedObject)self;
  int result;

  lseek(this->fd,self->readptr,0);
  result = read(this->fd, buffer, length);
  if(result < 0) {
    RaiseError(EIOError, "Unable to read from %s (%s)", URNOF(self), strerror(errno));
    return -1;
  };

  self->readptr += result;

  return result;
};

static int FileBackedObject_write(FileLikeObject self, char *buffer, unsigned long int length) {
  FileBackedObject this = (FileBackedObject)self;
  int result;

  lseek(this->fd,self->readptr,0);
  result = write(this->fd, buffer, length);
  if(result < 0) {
    RaiseError(EIOError, "Unable to write to %s (%s)", URNOF(self), strerror(errno));
  };

  self->readptr += result;

  self->size = max(self->size, self->readptr);

  return result;
};

static uint64_t FileLikeObject_tell(FileLikeObject self) {
  return self->readptr;
};

static void FileLikeObject_close(FileLikeObject self) {
  CALL(oracle, set, URNOF(self), AFF4_SIZE, &self->size, RESOLVER_DATA_UINT64);
  //talloc_free(self);
};

static void FileBackedObject_close(FileLikeObject self) {
  FileBackedObject this=(FileBackedObject)self;

  close(this->fd);
  talloc_free(self);
};

static uint64_t FileBackedObject_seek(FileLikeObject self, int64_t offset, int whence) {
  FileBackedObject this = (FileBackedObject)self;

  int64_t result = lseek(this->fd, offset, whence);
  
  if(result < 0) {
    DEBUG("Error seeking %s\n", strerror(errno));
    result = 0;
  };

  self->readptr = result;
  return result;
};

static AFFObject FileBackedObject_finish(AFFObject self) {
  FileBackedObject this = (FileBackedObject)self;
  char *urn = self->urn;
 
  return (AFFObject)this->Con(this, urn, 'w');
};

static char *FileLikeObject_get_data(FileLikeObject self) {
  char *data;

  if(self->size > MAX_CACHED_FILESIZE)
    goto error;

  if(self->data) return self->data;

  CALL(self, seek, 0,0);
  data = talloc_size(self, self->size+BUFF_SIZE);
  memset(data, 0, self->size+BUFF_SIZE);
  CALL(self, read, data, self->size);
  self->data = data;

  return self->data;

 error:
  return NULL;
};

VIRTUAL(FileLikeObject, AFFObject)
     VMETHOD(seek) = FileLikeObject_seek;
     VMETHOD(tell) = FileLikeObject_tell;
     VMETHOD(close) = FileLikeObject_close;
     VMETHOD(truncate) = FileLikeObject_truncate;
     VMETHOD(get_data) = FileLikeObject_get_data;

     VATTR(data) = NULL;
END_VIRTUAL

static int FileBackedObject_truncate(FileLikeObject self, uint64_t offset) {
  FileBackedObject this=(FileBackedObject)self;

  ftruncate(this->fd, offset);
  return this->__super__->truncate(self, offset);
};

/** A file backed object extends FileLikeObject */
VIRTUAL(FileBackedObject, FileLikeObject)
     VMETHOD(Con) = FileBackedObject_Con;
     VMETHOD(super.super.Con) = FileBackedObject_AFFObject_Con;
     VMETHOD(super.super.finish) = FileBackedObject_finish;

     VMETHOD(super.read) = FileBackedObject_read;
     VMETHOD(super.write) = FileBackedObject_write;
     VMETHOD(super.close) = FileBackedObject_close;
     VMETHOD_BASE(FileLikeObject, seek) = FileBackedObject_seek;
     VMETHOD(super.truncate) = FileBackedObject_truncate;
END_VIRTUAL;

/** This is the constructor which will be used when we get
    instantiated as an AFFObject.
*/
static AFFObject ZipFile_AFFObject_Con(AFFObject self, char *urn, char mode) {
  ZipFile this = (ZipFile)self;

  self->mode = mode;

  if(urn) {
    // Ok, we need to create ourselves from a URN. We need a
    // FileLikeObject first. We ask the oracle what object should be
    // used as our underlying FileLikeObject:
    self->urn = (char *)CALL(oracle, resolve, self, urn, AFF4_STORED, RESOLVER_DATA_URN);
    // We have no idea where we are stored:
    if(!self->urn) {
      RaiseError(ERuntimeError, "Can not find the storage for Volume %s", urn);
      goto error;
    };

    // Call our other constructor to actually read this file:
    self = (AFFObject)this->Con((ZipFile)this, self->urn, mode);
  } else {
    // Call ZipFile's AFFObject constructor.
    this->__super__->Con(self, urn, mode);
  };

  return self;
 error:
  talloc_free(self);
  return NULL;
};

static AFFObject ZipFile_finish(AFFObject self) {
  ZipFile this = (ZipFile)self;
  char *file_urn = (char *)CALL(oracle, resolve, self, self->urn, AFF4_STORED,
				RESOLVER_DATA_URN);

  // Make sure the constructor knows we want to create a new Zip
  // file if one doesnt already exist.
  self->mode = 'w';

  if(!file_urn) {
    RaiseError(ERuntimeError, "Volume %s has no " NAMESPACE "stored property?", self->urn);
    return NULL;
  };
    
  return (AFFObject)CALL((ZipFile)this, Con, file_urn, 'w');
};

// Seeks fd to start of CD
static int find_cd(ZipFile self, FileLikeObject fd, int directory_offset) {
  if(self->end->offset_of_cd != -1) {
    CALL(fd, seek, self->end->offset_of_cd, SEEK_SET);
    self-> total_entries = self->end->total_entries_in_cd_on_disk;
    // Its a Zip64 file...
  } else {
    struct Zip64CDLocator locator;
    struct Zip64EndCD end_cd;

    // We reposition ourselved just before the EndCentralDirectory to
    // find the locator:
    CALL(fd, seek, directory_offset - sizeof(locator), 0);
    CALL(fd, read, (char *)&locator, sizeof(locator));

    if(locator.magic != 0x07064b50) goto error;

    if(locator.disk_with_cd != 0 || locator.number_of_disks != 1) {
      RaiseError(ERuntimeError, "Zip Files with multiple parts are not supported");
      goto error;
    };

    // Now the Zip64EndCD
    CALL(fd, seek, locator.offset_of_end_cd, SEEK_SET);
    CALL(fd, read, (char *)&end_cd, sizeof(end_cd));

    if(end_cd.magic != 0x06064b50) goto error;
    
    if(end_cd.number_of_disk != 0 || end_cd.number_of_disk_with_cd != 0 ||
       end_cd.number_of_entries_in_volume != end_cd.number_of_entries_in_total) {
      RaiseError(ERuntimeError, "Zip Files with multiple parts are not supported");
      goto error;
    };

    self->total_entries = end_cd.number_of_entries_in_total;

    CALL(fd, seek, end_cd.offset_of_cd, SEEK_SET);
  };

  directory_offset = CALL(fd, tell);
  return directory_offset;
 error:
  return 0;
};

// Parses the extra field populating ourselves as needed. We should be
// positioned at the start of the extra field.
static int parse_extra_field(ZipFile self, FileLikeObject fd,unsigned int length) {
  uint16_t type,rec_length;

  if(length < 8) return 0;

  length -= CALL(fd, read, (char *)&type, sizeof(type));
  if(type != 1) goto error;

  length -= CALL(fd, read, (char *)&rec_length, sizeof(rec_length));

  if(length < rec_length) goto error;
  switch(rec_length) {
  case 24:
    length -= READ_INT(fd, self->original_member_size);
    length -= READ_INT(fd, self->compressed_member_size);
    length -= READ_INT(fd, self->offset_of_member_header);
    break;

  case 16:
    length -= READ_INT(fd, self->original_member_size);
    length -= READ_INT(fd, self->compressed_member_size);
    break;

  case 8:
    length -= READ_INT(fd, self->original_member_size);
  case 0:
    break;

  default:
    RaiseError(ERuntimeError, "Invalid extra record length");
    goto error;
  };

  CALL(fd, seek, length, SEEK_CUR);
  return 1;
 error:
  CALL(fd, seek, length, SEEK_CUR);
  return 0;
};

static ZipFile ZipFile_Con(ZipFile self, char *fd_urn, char mode) {
  char buffer[BUFF_SIZE+1];
  int length,i;
  FileLikeObject fd=NULL;
  uint64_t directory_offset=0;

  memset(buffer,0,BUFF_SIZE+1);

  // Make sure we have a URN
  CALL((AFFObject)self, Con, NULL, mode);

  // Is there a file we need to read?
  fd = (FileLikeObject)CALL(oracle, open, fd_urn, mode);
  if(!fd) {
    RaiseError(ERuntimeError, "Unable to open %s", fd_urn);
    goto error;
  };

  self->parent_urn = talloc_strdup(self, fd_urn);

  // FIXME - check the directory_offset - what should we do if its not
  // valid? We should check the signatures.

  // Is there a directory_offset and does it make sense?
  if(CALL(oracle, resolve2, URNOF(self), AFF4_DIRECTORY_OFFSET,
	  AS_BUFFER(directory_offset),
	  RESOLVER_DATA_UINT64) &&  directory_offset < fd->size) {
    goto exit;
  };

  // Find the End of Central Directory Record - We read about 4k of
  // data and scan for the header from the end, just in case there is
  // an archive comment appended to the end
  directory_offset = CALL(fd, seek, -(int64_t)BUFF_SIZE, SEEK_END);
  length = CALL(fd, read, buffer, BUFF_SIZE);

  // Non fatal Error occured
  if(length<0) 
    goto exit;

  // Scan the buffer backwards for an End of Central Directory magic
  for(i=length; i>0; i--) {
    if(*(uint32_t *)(buffer+i) == 0x6054b50) {
      break;
    };
  };
  
  if(i!=0) {
    // This is now the offset to the end of central directory record
    directory_offset += i;
    self->end = (struct EndCentralDirectory *)talloc_memdup(self, buffer+i, 
							    sizeof(*self->end));
    int j=0;

    // Is there a comment field? We expect the comment field to be
    // exactly a URN. If it is we can update our notion of the URN to
    // be the same as that.
    if(self->end->comment_len == strlen(URNOF(self))) {
      char *comment = buffer + i + sizeof(*self->end);
      // Is it a fully qualified name?
      if(!memcmp(comment, ZSTRING_NO_NULL(FQN))) {
	// Update our URN from the comment.
	memcpy(URNOF(self),comment, strlen(URNOF(self)));
      };
    };

    // Make sure that the oracle knows about this volume:
    // FIXME: should be Volatile?
    // Note that our URN has changed above which means we can not set
    // any resolver properties until now that our URN is finalised.
    CALL(oracle, set, URNOF(self), AFF4_STORED, URNOF(fd), RESOLVER_DATA_URN);
    CALL(oracle, add, URNOF(fd), AFF4_CONTAINS, URNOF(self), RESOLVER_DATA_URN, 1);

    // Find the CD
    if(!find_cd(self, fd, directory_offset)) goto error_reason;

    while(j < self->total_entries) {
      struct CDFileHeader cd_header;
      char *filename;
      char *escaped_filename;
      uint32_t tmp;

      // The length of the struct up to the filename
      // Only read up to the filename member
      if(sizeof(cd_header) != 
	 CALL(fd, read, (char *)&cd_header, sizeof(cd_header)))
	goto error_reason;

      // Does the magic match?
      if(cd_header.magic != 0x2014b50)
	goto error_reason;

      // Now read the filename
      escaped_filename = talloc_array(self, char, cd_header.file_name_length+1);
						
      if(CALL(fd, read, escaped_filename, cd_header.file_name_length) != 
	 cd_header.file_name_length)
	goto error_reason;

      // Unescape the filename
      filename = unescape_filename(self, escaped_filename);
      // This is the fully qualified name
      filename = fully_qualified_name(filename, filename, URNOF(self));

      // Tell the oracle about this new member
      CALL(oracle, set, filename, AFF4_STORED, URNOF(self), RESOLVER_DATA_URN);
      CALL(oracle, set, filename, AFF4_TYPE, AFF4_SEGMENT, RESOLVER_DATA_STRING);
      CALL(oracle, add, URNOF(self), AFF4_CONTAINS, filename, RESOLVER_DATA_URN, 1);

      // Parse the time from the CD
      {
	struct tm x = {
	  .tm_year = (cd_header.dosdate>>9) + 1980 - 1900,
	  .tm_mon = ((cd_header.dosdate>>5) & 0xF) -1,
	  .tm_mday = cd_header.dosdate & 0x1F,
	  .tm_hour = cd_header.dostime >> 11,
	  .tm_min = (cd_header.dostime>>5) & 0x3F, 
	  .tm_sec = (cd_header.dostime&0x1F) * 2,
	  .tm_zone = "GMT"
	};
	int64_t now = mktime(&x);
	if(now > 0)
	  CALL(oracle, set, filename, AFF4_TIMESTAMP, &now, RESOLVER_DATA_UINT64);
      };

      parse_extra_field(self, fd, cd_header.extra_field_len);

      CALL(oracle, set, filename, AFF4_VOLATILE_COMPRESSION, 
	   &cd_header.compression_method, RESOLVER_DATA_UINT16);

      CALL(oracle, set, filename, AFF4_VOLATILE_CRC, 
	   &cd_header.crc32, RESOLVER_DATA_UINT32);

      // The following checks for zip64 values
      tmp = cd_header.file_size == -1 ? self->original_member_size : cd_header.file_size;
      CALL(oracle, set, filename, AFF4_SIZE, 
	   &tmp, RESOLVER_DATA_UINT32);

      tmp = cd_header.compress_size == -1 ? self->compressed_member_size 
	: cd_header.compress_size;
      CALL(oracle, set, filename, AFF4_VOLATILE_COMPRESSED_SIZE, 
	   &tmp, RESOLVER_DATA_UINT32);
      
      tmp = cd_header.relative_offset_local_header == -1 ? self->offset_of_member_header 
	: cd_header.relative_offset_local_header;
      CALL(oracle, set, filename, AFF4_VOLATILE_HEADER_OFFSET, 
	   &tmp, RESOLVER_DATA_UINT32);
      self->offset_of_member_header = tmp;

      // Read the zip file itself
      {
	// Skip the comments - we dont care about them
	uint64_t current_offset = CALL(fd, seek, cd_header.file_comment_length, SEEK_CUR);
	struct ZipFileHeader file_header;
	uint32_t file_offset;

	CALL(fd,seek, self->offset_of_member_header, SEEK_SET);
	CALL(fd, read, (char *)&file_header, sizeof(file_header));

	file_offset = self->offset_of_member_header +
	  sizeof(file_header) +
	  file_header.file_name_length + file_header.extra_field_len;

	CALL(oracle, set, filename, AFF4_VOLATILE_FILE_OFFSET,
	     &file_offset, RESOLVER_DATA_UINT32);

	CALL(fd, seek, current_offset, SEEK_SET);
      };

      // Is this file a properties file?
      {
	int filename_length = strlen(filename);
	int properties_length = strlen("properties");
	int len;

	// We identify streams by their filename ending with "properties"
	// and parse out their properties:
	if(filename_length >= properties_length && 
	   !strcmp("properties", filename + filename_length - properties_length)) {
	  char *text = CALL(self, read_member, self, filename, &len);
	  char *context = dirname(filename);

	  //	  printf("Found property file %s\n%s", filename, text);

	  if(text) {
	    CALL(oracle, parse, context, text, len);
	    talloc_free(text);
	  };
	};
      };

      // Do we have as many CDFileHeaders as we expect?
      j++;
      talloc_free(escaped_filename);
    };   
  } else {
    // A central directory was not found, but we want to open this
    // file in read mode - this means it is not a zip file.
    if(mode == 'r') goto error_reason;
  };

  CALL(oracle, set, URNOF(self), AFF4_DIRECTORY_OFFSET, 
       &directory_offset, RESOLVER_DATA_UINT64);
 exit:
  CALL(oracle, set, URNOF(self), AFF4_TYPE, AFF4_ZIP_VOLUME, RESOLVER_DATA_STRING);

  CALL(oracle, cache_return, (AFFObject)fd);
  return self;

 error_reason:
  RaiseError(EInvalidParameter, "%s is not a zip file", URNOF(fd));
 error:
  if(fd) {
    CALL(oracle, cache_return, (AFFObject)fd);
  };
  talloc_free(self);
  return NULL;
};

/*** NOTE - The buffer callers receive will not be owned by the
     callers. Callers should never free it nor steal it. The buffer is
     owned by the cache system and callers merely borrow a reference
     to it. length will be adjusted to the size of the buffer.
*/
static char *ZipFile_read_member(ZipFile self, void *ctx,
				 char *filename, 
				 int *length) {
  char *buffer;
  FileLikeObject fd;

  uint32_t file_size = 0;
  uint16_t compression_method = ZIP_STORED;
  uint64_t file_offset = 0;
  char volume_urn[BUFF_SIZE];
  char fd_urn[BUFF_SIZE];

  CALL(oracle, resolve2, filename, AFF4_SIZE, 
       AS_BUFFER(file_size),
       RESOLVER_DATA_UINT64);
  
  CALL(oracle, resolve2, filename, AFF4_VOLATILE_COMPRESSION,
       AS_BUFFER(compression_method),
       RESOLVER_DATA_UINT16);
  
  CALL(oracle, resolve2, filename, AFF4_VOLATILE_FILE_OFFSET,
       AS_BUFFER(file_offset),
       RESOLVER_DATA_UINT64);
  
  // This is the volume the filename is stored in
  CALL(oracle, resolve2, filename, AFF4_STORED,
       volume_urn, BUFF_SIZE,
       RESOLVER_DATA_URN);

  // This is the file that backs this volume
  CALL(oracle, resolve2, volume_urn, AFF4_STORED,
       fd_urn, BUFF_SIZE,
       RESOLVER_DATA_URN);
  
  fd = (FileLikeObject)CALL(oracle, open, fd_urn, 'r');
  if(!fd) goto exit;

  // We know how large we would like the buffer so we make it that big
  // (bit extra for null termination).
  buffer = talloc_size(ctx, file_size + 2);
  *length = file_size;

  // Go to the start of the file
  CALL(fd, seek, file_offset, SEEK_SET);

  if(compression_method == ZIP_DEFLATE) {
    uint32_t compressed_length = *length * 2;
    uint32_t uncompressed_length = *length;
    char *tmp;
    z_stream strm;
    
    CALL(oracle, resolve2, filename, AFF4_VOLATILE_COMPRESSED_SIZE,
	 AS_BUFFER(compressed_length),
	 RESOLVER_DATA_UINT32);

    tmp = talloc_size(buffer, compressed_length);
    memset(&strm, 0, sizeof(strm));

    //Now read the data in
    if(CALL(fd, read, tmp, compressed_length) != compressed_length)
      goto error;

    // Decompress it
    /** Set up our decompressor */
    strm.next_in = (unsigned char *)tmp;
    strm.avail_in = compressed_length;
    strm.next_out = (unsigned char *)buffer;
    strm.avail_out = uncompressed_length;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;

    if(inflateInit2(&strm, -15) != Z_OK) {
      RaiseError(ERuntimeError, "Failed to initialise zlib");
      goto error;
    };

    if(inflate(&strm, Z_FINISH) !=Z_STREAM_END || \
       strm.total_out != uncompressed_length) {
      RaiseError(ERuntimeError, "Failed to fully decompress chunk (%s)", strm.msg);
      goto error;
    };

    inflateEnd(&strm);
    // AFF4 pretty much always uses ZIP_STORED for everything:
  } else if(compression_method == ZIP_STORED) {
    if(CALL(fd, read, buffer, *length) != *length) {
      RaiseError(EIOError, "Unable to read %d bytes from %s@%lld", *length, 
		 URNOF(fd), fd->readptr);
      goto error;
    };

  } else {
    RaiseError(EInvalidParameter, "Compression type %X unknown", compression_method);
    goto error;
  };

  // Here we have a good buffer - now calculate the crc32
  if(1){
    uLong crc = crc32(0L, Z_NULL, 0);
    uint32_t file_crc = 0;

    CALL(oracle, resolve2, filename, AFF4_VOLATILE_CRC,
	 &file_crc, sizeof(file_crc),
	 RESOLVER_DATA_UINT32);

    crc = crc32(crc, (unsigned char *)buffer,
		file_size);

    if(crc != file_crc) {
      RaiseError(EIOError, 
		 "CRC not matched on decompressed file %s", filename);
      goto error;
    };
  };

  CALL(oracle, cache_return, (AFFObject)fd);
  return buffer;
 error:
  CALL(oracle, cache_return, (AFFObject)fd);
  talloc_free(buffer);
  
 exit:
  return NULL;
};

static FileLikeObject ZipFile_open_member(ZipFile self, char *filename, char mode,
				   char *extra, uint16_t extra_field_len,
				   uint16_t compression) {
  FileLikeObject result=NULL;
  char *ctx=talloc_size(NULL, 1);

  switch(mode) {
  case 'w': {
    struct ZipFileHeader header;
    FileLikeObject fd;
    // We start writing new files at this point
    uint64_t directory_offset=0;
    char *escaped_filename;
    time_t epoch_time = time(NULL);
    struct tm *now = localtime(&epoch_time);

    // Make sure the filename we write on the archive is relative, and
    // the filename we use with the resolver is fully qualified
    escaped_filename = escape_filename(self, relative_name(self, filename, URNOF(self)));
    filename = fully_qualified_name(escaped_filename, filename, URNOF(self));

    CALL(oracle, resolve2, URNOF(self), AFF4_DIRECTORY_OFFSET,
	 &directory_offset, sizeof(directory_offset),
	 RESOLVER_DATA_UINT64);

    // Indicate that the file is dirty - This means we will be writing
    // a new CD on it
    CALL(oracle, set, URNOF(self), AFF4_VOLATILE_DIRTY, "1", RESOLVER_DATA_STRING);

    // Open our current volume for writing:
    fd = (FileLikeObject)CALL(oracle, open, self->parent_urn, 'w');
    if(!fd) goto error;

    // Go to the start of the directory_offset
    CALL(fd, seek, directory_offset, SEEK_SET);
    DEBUG("seeking %p to %lld (%lld)\n", fd, directory_offset, fd->size);

    // Write a file header on
    memset(&header, 0, sizeof(header));
    header.magic = 0x4034b50;
    header.version = 0x14;
    // We prefer to write trailing directory structures
    header.flags = 0x08;
    header.compression_method = compression;
    header.file_name_length = strlen(escaped_filename);
    header.extra_field_len = extra_field_len;
    header.lastmoddate = (now->tm_year + 1900 - 1980) << 9 | 
      (now->tm_mon + 1) << 5 | now->tm_mday;
    header.lastmodtime = now->tm_hour << 11 | now->tm_min << 5 | 
      now->tm_sec / 2;

    CALL(fd, write,(char *)&header, sizeof(header));
    CALL(fd, write, ZSTRING_NO_NULL(escaped_filename));

    CALL(oracle, set, filename, AFF4_VOLATILE_COMPRESSION,
	 &compression, RESOLVER_DATA_UINT16);
    {
      uint64_t offset = CALL(fd, tell);
 
      CALL(oracle, set, filename, AFF4_VOLATILE_FILE_OFFSET,
	   &offset, RESOLVER_DATA_UINT64);
    };

    CALL(oracle, set, filename, AFF4_VOLATILE_HEADER_OFFSET,
	 &directory_offset, RESOLVER_DATA_UINT64);

    result = (FileLikeObject)CONSTRUCT(ZipFileStream, 
				       ZipFileStream, Con, self, 
				       filename, self->parent_urn, 
				       URNOF(self),
				       'w', fd);
    break;
  };
  case 'r': {
    // Check that this volume actually contains the requested member:
    if(!CALL(oracle, is_set, URNOF(self), AFF4_CONTAINS, filename)) {
      RaiseError(ERuntimeError, "Volume does not contain member %s", filename);
      goto error;
    };

    if(compression != ZIP_STORED) {
      RaiseError(ERuntimeError, "Unable to open seekable member for compressed members.");
      break;
    };

    result = (FileLikeObject)CONSTRUCT(ZipFileStream, 
				       ZipFileStream, Con, self, 
				       filename, self->parent_urn,
				       URNOF(self),
				       'r', NULL);
    break;
  };

  default:
    RaiseError(ERuntimeError, "Unsupported mode '%c'", mode);
    goto error;
  };

  talloc_free(ctx);
  return result;

 error:
  talloc_free(ctx);
  return NULL;
};

/** This writes a zip64 end of central directory and a central
    directory locator */
static void write_zip64_CD(ZipFile self, FileLikeObject fd, 
			   uint64_t directory_offset, int total_entries) {
  struct Zip64CDLocator locator;
  struct Zip64EndCD end_cd;

  locator.magic = 0x07064b50;
  locator.disk_with_cd = 0;
  locator.offset_of_end_cd = CALL(fd, tell);
  locator.number_of_disks = 1;

  memset(&end_cd, 0, sizeof(end_cd));
  end_cd.magic = 0x06064b50;
  end_cd.size_of_header = sizeof(end_cd)-12;
  end_cd.version_made_by = 0x2d;
  end_cd.version_needed = 0x2d;
  end_cd.number_of_entries_in_volume = total_entries;
  end_cd.number_of_entries_in_total = total_entries;
  end_cd.size_of_cd = locator.offset_of_end_cd - directory_offset;
  end_cd.offset_of_cd = directory_offset;
  
  DEBUG("writing ECD at %lld\n", fd->readptr);
  CALL(fd,write, (char *)&end_cd, sizeof(end_cd));
  CALL(fd,write, (char *)&locator, sizeof(locator));
};

static void dump_volume_properties(ZipFile self) {
  char *properties = CALL(oracle, export_urn, URNOF(self), URNOF(self));

  if(strlen(properties)>0) {
    CALL(self, writestr, "properties", ZSTRING_NO_NULL(properties), 
	 NULL, 0, ZIP_STORED);
  };

  talloc_free(properties);
};


static void ZipFile_close(ZipFile self) {
  // Dump the current CD. We expect our fd is seeked to the right
  // place:
  int k=0;
  char _didModify;

  // We iterate over all the items which are contained in the
  // volume. We then write them into the CD.
  if(CALL(oracle, resolve2, URNOF(self), AFF4_VOLATILE_DIRTY,
	  AS_BUFFER(_didModify),
	  RESOLVER_DATA_STRING)) {
    struct EndCentralDirectory end;
    FileLikeObject fd;
    uint64_t directory_offset=0;
    uint64_t tmp;

    // Write a properties file if needed
    //dump_volume_properties(self);

    fd = (FileLikeObject)CALL(oracle, open, self->parent_urn, 'w');
    if(!fd) return;
    
    CALL(oracle, resolve2, URNOF(self), AFF4_DIRECTORY_OFFSET,
	 &directory_offset, sizeof(directory_offset),
	 RESOLVER_DATA_UINT64);

    CALL(fd, seek, directory_offset, SEEK_SET);
    
    // Dump the central directory for this volume
    {
      // Get all the URNs contained within this volume
      RESOLVER_ITER iter;
      char urn[BUFF_SIZE];
      StringIO zip64_header = CONSTRUCT(StringIO, StringIO, Con, NULL);
      
      CALL(zip64_header, write, "\x01\x00\x00\x00", 4);

      // Iterate over all the AFF4_CONTAINS URNs
      CALL(oracle, get_iter, &iter, URNOF(self), AFF4_CONTAINS, RESOLVER_DATA_URN);
      while(CALL(oracle, iter_next, &iter, urn, BUFF_SIZE)) {
	struct CDFileHeader cd;
	// We use type to anchor temporary allocations
	char type[BUFF_SIZE];
	time_t epoch_time=0;
	struct tm *now;

	// This gets the relative name of the fqn
	char *escaped_filename = escape_filename(self, 
		       relative_name(self, urn, URNOF(self)));	

	CALL(oracle, resolve2, urn, AFF4_TIMESTAMP,
	     AS_BUFFER(epoch_time),
	     RESOLVER_DATA_UINT32);

	now = localtime(&epoch_time);

	// Only store segments here
	if(CALL(oracle, resolve2, urn, AFF4_TYPE,
		type, BUFF_SIZE,
		RESOLVER_DATA_URN) && strcmp(type, AFF4_SEGMENT))
	  continue;
	
	// Clear temporary data
	CALL(zip64_header, truncate, 4);
	
	// Prepare a central directory record
	memset(&cd, 0, sizeof(cd));
	cd.magic = 0x2014b50;
	cd.version_made_by = 0x317;
	cd.version_needed = 0x14;
	cd.compression_method = ZIP_STORED;
	
	CALL(oracle, resolve2, urn, AFF4_VOLATILE_COMPRESSION,
	     &cd.compression_method, sizeof(cd.compression_method),
	     RESOLVER_DATA_UINT16);
	
	// We always write trailing directory structures
	cd.flags = 0x8;
	cd.crc32 = 0;
	
	CALL(oracle, resolve2, urn, AFF4_VOLATILE_CRC,
	     &cd.crc32, sizeof(cd.crc32),
	     RESOLVER_DATA_UINT32);
	
	cd.dosdate = (now->tm_year + 1900 - 1980) << 9 | 
	  (now->tm_mon + 1) << 5 | now->tm_mday;
	cd.dostime = now->tm_hour << 11 | now->tm_min << 5 | 
	  now->tm_sec / 2;
	cd.external_file_attr = 0644 << 16L;
	cd.file_name_length = strlen(escaped_filename);
	
	// The following are optional zip64 fields. They must appear
	// in this order:
	CALL(oracle, resolve2, urn, AFF4_SIZE,
	     &tmp, sizeof(tmp),
	     RESOLVER_DATA_UINT64);

	if(tmp > ZIP64_LIMIT) {
	  cd.file_size = -1;
	  CALL(zip64_header, write, (char *)&tmp, sizeof(tmp));
	} else {
	  cd.file_size = tmp;
	}
	
	// AFF4 does not support very large segments since they are
	// unseekable.
	CALL(oracle, resolve2, urn, AFF4_VOLATILE_COMPRESSED_SIZE,
	     AS_BUFFER(cd.compress_size),
	     RESOLVER_DATA_UINT32);
	
	tmp=0;	
	CALL(oracle, resolve2, urn, AFF4_VOLATILE_HEADER_OFFSET,
	     &tmp, sizeof(tmp),
	     RESOLVER_DATA_UINT64);

	if(tmp > ZIP64_LIMIT) {
	  cd.relative_offset_local_header = -1;
	  CALL(zip64_header, write, (char *)&tmp, sizeof(tmp));
	} else {
	  cd.relative_offset_local_header = tmp;
	};
	
	// We need to append an extended zip64 header
	if(zip64_header->size > 4) {
	  uint16_t *x = (uint16_t *)(zip64_header->data +2);
	  
	  // update the size of the extra field
	  *x=zip64_header->size-4;
	  cd.extra_field_len = zip64_header->size;
	};
	
	// OK - write the cd header
	CALL(fd, write, (char *)&cd, sizeof(cd));
	CALL(fd, write, (char *)ZSTRING_NO_NULL(escaped_filename));
	if(zip64_header->size > 4) {
	  CALL(fd, write, zip64_header->data, zip64_header->size);
	};

	k++;
      };

      talloc_free(zip64_header);
    };

    // Now write an end of central directory record
    memset(&end, 0, sizeof(end));
    end.magic = 0x6054b50;
    end.size_of_cd = CALL(fd, tell) - directory_offset;

    if(directory_offset > ZIP64_LIMIT) {
      end.offset_of_cd = -1;
      write_zip64_CD(self, fd, directory_offset, k);
    } else {
      end.offset_of_cd = directory_offset;
    };
    
    end.total_entries_in_cd_on_disk = k;
    end.total_entries_in_cd = k;
    end.comment_len = strlen(URNOF(self));

    // Make sure to add our URN to the comment field in the end
    CALL(fd, write, (char *)&end, sizeof(end));
    CALL(fd, write, ZSTRING_NO_NULL(URNOF(self)));

    // Unlock the file
    CALL(oracle, cache_return, (AFFObject)fd);
    
    // Close the fd
    CALL(fd, close);
  };
};

/** This is just a convenience function - real simple now */
static int ZipFile_writestr(ZipFile self, char *filename, 
		      char *data, int len, char *extra, int extra_len, 
		      uint16_t compression) {
  FileLikeObject fd = CALL(self, open_member, filename, 'w', extra, extra_len,
			   compression);
  if(fd) {
    len = CALL(fd, write, data, len);
    CALL(fd, close);

    return len;
  } else return -1;
};

VIRTUAL(ZipFile, AFFObject)
     VMETHOD(Con) = ZipFile_Con;
     VMETHOD(read_member) = ZipFile_read_member;
     VMETHOD(open_member) = ZipFile_open_member;
     VMETHOD(close) = ZipFile_close;
     VMETHOD(writestr) = ZipFile_writestr;
     VMETHOD(super.Con) = ZipFile_AFFObject_Con;
     VMETHOD(super.finish) = ZipFile_finish;
END_VIRTUAL

/** 
    ZipFileStream objects may not expire until they are ready - this
    is because you cant really recreate them. Ideally they should not
    exist for long anyway.
*/

static int ZipFileStream_destroy(void *self) {
  return -1;
};

/** 
    NOTE - this object must only even be obtained through
    ZipFile.open_member. 
    
    If the object is opened for writing, the container_fd is retained
    and locked until this object is closed, since it is impossible to
    write to the container while this specific stream is still opened
    for writing.

    You must write to the segment as quickly as possible and close it
    immediately - do not return this to the oracle cache (since it was
    not obtained through oracle.open()).
    
    If the segment is opened for reading the underlying file is not
    locked, and multiple segments may be opened for reading at the
    same time.
    
    container_urn is the URN of the ZipFile container which holds this
    member, file_urn is the URN of the backing FileLikeObject which
    the zip file is written on. filename is the filename of this new
    zip member.

*/
static ZipFileStream ZipFileStream_Con(ZipFileStream self, char *filename, 
				       char *file_urn, char *container_urn,
				       char mode, FileLikeObject file_fd) {
  char *fqn_filename = fully_qualified_name(self, filename, URNOF(self));

  ((AFFObject)self)->mode = mode;

  self->file_urn = talloc_strdup(self, file_urn);
  self->container_urn = talloc_strdup(self, container_urn);

  EVP_DigestInit(&self->digest, EVP_sha256());
  //EVP_DigestInit(&self->digest, EVP_md5());

  // If we are opened for writing we need to lock the file_urn so
  // noone else can write on top of us. We do this by retaining a
  // reference to the volume fd and not returning it to the resolver
  // until we are closed.
  if(mode=='w') {
    self->file_fd = file_fd;
  };

  if(!CALL(oracle, resolve2, fqn_filename, AFF4_VOLATILE_COMPRESSION,
	   &self->compression, sizeof(self->compression),
	   RESOLVER_DATA_UINT16) ||
     
     !CALL(oracle, resolve2, fqn_filename, AFF4_VOLATILE_FILE_OFFSET,
	   &self->file_offset, sizeof(self->file_offset),
	   RESOLVER_DATA_UINT64)
     ) {
    // We fail here because we dont know the compression or file
    // offset where we are supposed to begin.
    RaiseError(ERuntimeError, "Unable to resolve parameters for ZipFileStream %s", 
	       filename);
    goto error;
  };

  self->super.size = 0;
  CALL(oracle, resolve2, fqn_filename, AFF4_SIZE,
       AS_BUFFER(self->super.size),
       RESOLVER_DATA_UINT64);
    
  URNOF(self) = talloc_strdup(self, filename);
  DEBUG("ZipFileStream: created %s\n", URNOF(self));

  if(mode=='w' && self->compression == ZIP_DEFLATE) {
    // Initialise the stream compressor
    memset(&self->strm, 0, sizeof(self->strm));
    self->strm.next_in = talloc_size(self, BUFF_SIZE);
    self->strm.next_out = talloc_size(self, BUFF_SIZE);

    if(deflateInit2(&self->strm, 9, Z_DEFLATED, -15,
		    9, Z_DEFAULT_STRATEGY) != Z_OK) {
      RaiseError(ERuntimeError, "Unable to initialise zlib (%s)", self->strm.msg);
      goto error;
    };
  };

  talloc_set_destructor((void *)self, ZipFileStream_destroy);
  // Set important parameters in the zinfo
  return self;

 error:
  talloc_free(self);
  return NULL;
};

/**
   This zlib trickery comes from http://www.zlib.net/zlib_how.html
**/
static int ZipFileStream_write(FileLikeObject self, char *buffer, unsigned long int length) {
  ZipFileStream this = (ZipFileStream)self;
  int result=0;

  // Update the crc:
  this->crc32 = crc32(this->crc32, 
		      (unsigned char*)buffer,
		      length);

  // Update the sha1:
  EVP_DigestUpdate(&this->digest,(const unsigned char *)buffer,length);

  // Is this compressed?
  if(this->compression == ZIP_DEFLATE) {
    unsigned char compressed[BUFF_SIZE];

    /** Give the buffer to zlib */
    this->strm.next_in = (unsigned char *)buffer;
    this->strm.avail_in = length;

    /** We spin here until zlib consumed all the data */
    do {
      int ret;

      this->strm.avail_out = BUFF_SIZE;
      this->strm.next_out = compressed;
      
      ret = deflate(&this->strm, Z_NO_FLUSH);
      ret = CALL(this->file_fd, write, (char *)compressed, 
		 BUFF_SIZE - this->strm.avail_out);
      if(ret<0) return ret;
      result += ret;
    } while(this->strm.avail_out == 0);

  } else {
    /** Without compression, we just write the buffer right away */
    DEBUG("(%p) writing data at 0x%llX\n", self, this->file_fd->readptr);
    result = CALL(this->file_fd, write, buffer, length);
    if(result<0) 
      goto exit;
  }; 

  /** Update our compressed size here */
  this->compress_size += result;

  /** The readptr and the size are advanced by the uncompressed amount
   */
  self->readptr += length;
  self->size = max(self->size, self->readptr);
  
  exit:
  return result;
};

static int ZipFileStream_read(FileLikeObject self, char *buffer,
			      unsigned long int length) {
  ZipFileStream this = (ZipFileStream)self;
  int len=0;
  FileLikeObject fd;

  // We only read as much data as there is
  length = min(length, self->size - self->readptr);

  if(this->compression == 0) {
    /** Position our write pointer */
    fd = (FileLikeObject)CALL(oracle, open, this->file_urn, 'r');
    CALL(fd, seek, this->file_offset + self->readptr, SEEK_SET);
    len = CALL(fd, read, buffer, length);
    self->readptr += len;
    CALL(oracle, cache_return, (AFFObject)fd);

    // We cheat here and decompress the entire member, then copy whats
    // needed out
  } else if(this->compression == ZIP_DEFLATE) {
    int offset;

    if(!self->data) {
      ZipFile container = (ZipFile)CALL(oracle, open, this->container_urn, 'r');
      int length;
      if(!container) return -1;

      self->data = CALL(container, read_member, self, URNOF(self), &length);
      self->size = length;
      CALL(oracle, cache_return, (AFFObject)container);
    };

    offset = min(self->readptr, self->size);
    len = max(self->readptr - offset, self->size);
    memcpy(buffer, self->data + offset, len);
  }
  return len;
};

static void ZipFileStream_close(FileLikeObject self) {
  ZipFileStream this = (ZipFileStream)self;
  FileLikeObject fd;
  int magic = 0x08074b50;

  DEBUG("ZipFileStream: closed %s\n", URNOF(self));
  if(((AFFObject)self)->mode != 'w') {
    talloc_set_destructor((void *)self, NULL);
    talloc_free(self);
    return;
  };

  if(this->compression == ZIP_DEFLATE) {
    unsigned char compressed[BUFF_SIZE];

    do {
      int ret;

      this->strm.avail_out = BUFF_SIZE;
      this->strm.next_out = compressed;
      
      ret = deflate(&this->strm, Z_FINISH);
      ret = CALL(this->file_fd, write, (char *)compressed, 
		 BUFF_SIZE - this->strm.avail_out);
      self->readptr += ret;

      this->compress_size += ret;
    } while(this->strm.avail_out == 0);
    
    (void)deflateEnd(&this->strm);
  };

  // Store important information about this file
  CALL(oracle, add, this->container_urn, AFF4_CONTAINS, URNOF(self), RESOLVER_DATA_URN, 1);
  CALL(oracle, set, URNOF(self), AFF4_STORED, this->container_urn, RESOLVER_DATA_URN);
  {
    uint32_t tmp = time(NULL);
    CALL(oracle, set, URNOF(self), AFF4_TIMESTAMP, &tmp, RESOLVER_DATA_UINT32);
  };

  CALL(oracle, set, URNOF(self), AFF4_SIZE, &self->size, RESOLVER_DATA_UINT64);
  CALL(oracle, set, URNOF(self), AFF4_VOLATILE_COMPRESSED_SIZE, 
       &this->compress_size, RESOLVER_DATA_UINT32);
  CALL(oracle, set, URNOF(self), AFF4_VOLATILE_CRC, &this->crc32, RESOLVER_DATA_UINT32);

  // Write a signature:
  CALL(this->file_fd, write, (char *)&magic, sizeof(magic));
  CALL(this->file_fd, write, (char *)&this->crc32,
       sizeof(this->crc32));

  // Zip64 data descriptor
  if(this->file_offset > ZIP64_LIMIT || this->compress_size > ZIP64_LIMIT
     || self->size > ZIP64_LIMIT) {
    CALL(this->file_fd, write, (char *)&this->compress_size, sizeof(uint64_t));
    CALL(this->file_fd, write, (char *)&self->size, sizeof(uint64_t));
  } else {
    // Regular data descriptor
    uint32_t size = self->size;
    uint32_t csize = this->compress_size;

    CALL(this->file_fd, write, (char *)&csize, sizeof(csize));
    CALL(this->file_fd, write, (char *)&size, sizeof(size));
  };

  // This is the point where we will be writing the next file - right
  // at the end of this file.
  CALL(oracle, set, this->container_urn, AFF4_DIRECTORY_OFFSET,
       &this->file_fd->readptr, RESOLVER_DATA_UINT64);

  printf("Setting to %s\n", from_int(this->file_fd->readptr));

  // Calculate the sha1 hash and set the hash in the resolver:
  {
    unsigned char digest[BUFF_SIZE];
    unsigned int digest_len=sizeof(digest);

    memset(digest, 0, BUFF_SIZE);
    EVP_DigestFinal(&this->digest,digest,&digest_len);
    if(digest_len < BUFF_SIZE) {
      TDB_DATA tmp;
      
      tmp.dptr = digest;
      tmp.dsize = digest_len;
      
      // Tell the resolver what the hash is:
      CALL(oracle, set, URNOF(self), AFF4_SHA, &tmp, RESOLVER_DATA_TDB_DATA);
    };
  };

  // Make sure the lock is removed from the underlying file:
  CALL(oracle, cache_return, (AFFObject)this->file_fd);
  talloc_set_destructor((void *)self, NULL);
  talloc_free(self);
};

static AFFObject ZipFileStream_AFFObject_Con(AFFObject self, char *urn, char mode) {
  if(urn) {
    char *volume_urn = (char *)CALL(oracle, resolve, self, urn, AFF4_STORED,
				    RESOLVER_DATA_URN);
    ZipFile parent;
    AFFObject result;
    uint16_t compression = ZIP_STORED;

    CALL(oracle, resolve2, urn, AFF4_COMPRESSION,
	 AS_BUFFER(compression),
	 RESOLVER_DATA_UINT16);

    if(!volume_urn) {
      RaiseError(ERuntimeError, "Parent not set?");
      goto error;
    };

    // Open the volume:
    parent = (ZipFile)CALL(oracle, open, volume_urn, mode);
    // Now just return the member from the volume:
    talloc_free(self);
    result = (AFFObject)CALL(parent, open_member, urn, mode, NULL, 0, compression);
    CALL(oracle, cache_return, (AFFObject)parent);

    return result;
  };

  return self;

 error:
  talloc_free(self);
  return NULL;

};


VIRTUAL(ZipFileStream, FileLikeObject)
     VMETHOD(super.super.Con) = ZipFileStream_AFFObject_Con;
     VMETHOD(Con) = ZipFileStream_Con;
     VMETHOD(super.write) = ZipFileStream_write;
     VMETHOD(super.read) = ZipFileStream_read;
     VMETHOD(super.close) = ZipFileStream_close;

// Initialise the encoding luts
     encode_init();
END_VIRTUAL

void print_cache(Cache self) {
  Cache i;

  list_for_each_entry(i, &self->cache_list, cache_list) {
    printf("%s %p %s\n",(char *) i->key,i->data, (char *)i->data);
  };
};

char *fully_qualified_name(void *ctx, char *filename, char *volume_urn) {
  char fqn_filename[BUFF_SIZE];

  if(!volume_urn || startswith(filename, FQN)) {
    return talloc_strdup(ctx, filename);
  } else {
    snprintf(fqn_filename, BUFF_SIZE, "%s/%s", volume_urn, filename);
    return talloc_strdup(ctx, fqn_filename);
  };
};

char *relative_name(void *ctx, char *name, char *volume_urn) {
  if(startswith(name, volume_urn)) {
    return talloc_strdup(ctx, name + strlen(volume_urn)+1);
  };

  return talloc_strdup(ctx,name);
};
