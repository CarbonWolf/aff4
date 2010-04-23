/*
** tsk3.c
** 
** Made by (mic)
** Login   <mic@laptop>
** 
** Started on  Fri Apr 16 10:01:04 2010 mic
** Last update Sun May 12 01:17:25 2002 Speed Blue
*/

#include "tsk3.h"


/** This macro is used to receive the object reference from a
    member of the type.
*/
#define GET_Object_from_member(type, object, member)                    \
  (type)(((char *)object) - (unsigned long)(&((type)0)->member))

void IMG_INFO_close(TSK_IMG_INFO *img) {
  Extended_TSK_IMG_INFO self = (Extended_TSK_IMG_INFO)img;

  CALL(self->container, close);
};

ssize_t IMG_INFO_read(TSK_IMG_INFO *img, TSK_OFF_T off, char *buf, size_t len) {
  Extended_TSK_IMG_INFO self = (Extended_TSK_IMG_INFO)img;

  return (ssize_t)CALL(self->container, read, (uint64_t)off, buf, len);
};

AFF4ImgInfo AFF4ImgInfo_Con(AFF4ImgInfo self, char *urn, TSK_OFF_T offset) {
  FileLikeObject fd;

  self->urn = new_RDFURN(self);
  CALL(self->urn, set, urn);

  self->offset = offset;

  // Try to open it for reading just to make sure its ok:
  fd = (FileLikeObject)CALL(oracle, open, self->urn, 'r');
  if(!fd) goto error;

  // Initialise the img struct with the correct callbacks:
  self->img = talloc_zero(self, struct Extended_TSK_IMG_INFO_t);
  self->img->container = self;

  self->img->base.read = IMG_INFO_read;
  self->img->base.close = IMG_INFO_close;
  self->img->base.size = fd->size->value;
  self->img->base.sector_size = 512;
  self->img->base.itype = TSK_IMG_TYPE_RAW_SING;

  CALL((AFFObject)fd, cache_return);

  return self;

 error:
  talloc_free(self);
  return NULL;
};

ssize_t AFF4ImgInfo_read(AFF4ImgInfo self, TSK_OFF_T off, OUT char *buf, size_t len) {
  FileLikeObject fd = (FileLikeObject)CALL(oracle, open, self->urn, 'r');

  if(fd) {
    ssize_t res;

    CALL(fd, seek, off, SEEK_SET);
    res = CALL(fd, read, buf, len);
    CALL((AFFObject)fd, cache_return);

    return res;
  };

  return -1;
};

// Dont really do anything here
void AFF4ImgInfo_close(AFF4ImgInfo self) {

};

VIRTUAL(AFF4ImgInfo, Object) {
  VMETHOD(Con) = AFF4ImgInfo_Con;
  VMETHOD(read) = AFF4ImgInfo_read;
  VMETHOD(close) = AFF4ImgInfo_close;
} END_VIRTUAL


int FS_Info_dest(void *this) {
  FS_Info self = (FS_Info)this;

  tsk_fs_close(self->fs);
  return 0;
};

static FS_Info FS_Info_Con(FS_Info self, AFF4ImgInfo img, TSK_FS_TYPE_ENUM type) {
  // Now try to open the filesystem
  self->fs = tsk_fs_open_img((struct TSK_IMG_INFO *)img->img, img->offset, type);
  if(!self->fs) {
    RaiseError(ERuntimeError, "Unable to open the image as a filesystem");
    goto error;
  };

  // Make sure that the filesystem is properly closed when we get freed
  talloc_set_destructor((void *)self, FS_Info_dest);
  return self;

 error:
  talloc_free(self);
  return NULL;
};

VIRTUAL(FS_Info, Object) {
  VMETHOD(Con) = FS_Info_Con;
} END_VIRTUAL
