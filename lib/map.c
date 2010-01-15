#include "aff4.h"

static RDFValue MapValue_Con(RDFValue self) {
  MapValue this = (MapValue)self;

  this->urn = new_RDFURN(self);
  this->number_of_points = 0;
  this->number_of_urns = 0;
  this->cache = CONSTRUCT(Cache, Cache, Con, self, 100, 0);
  this->points = talloc_array(self, struct map_point, 1);
  this->targets = talloc_array(self, RDFURN, 1);

  this->size = new_XSDInteger(self);
  this->target_period = new_XSDInteger(self);
  this->image_period = new_XSDInteger(self);

  // Set defaults
  this->image_period->value = -1;
  this->target_period->value = -1;

  return self;
};

/** Its far more efficient for us to reload ourselves from our
    underlying segment than to serialise into the tdb. We therefore
    just store the URN of the map object in the resolver.
*/
static TDB_DATA *MapValue_encode(RDFValue self) {
  TDB_DATA *result = talloc(self, TDB_DATA);

  result->dptr = (unsigned char *)CALL(self, serialise);
  if(!result->dptr) goto error;

  result->dsize = strlen((char *)result->dptr)+1;

  return result;

 error:
  talloc_free(result);
  return NULL;
};

static int MapValue_decode(RDFValue self, char *data, int length) {
  MapValue this = (MapValue)self;
  FileLikeObject fd;
  RDFURN segment = new_RDFURN(self);

  CALL(this->urn, set, data);

  // Load some parameters
  CALL(oracle, resolve_value, this->urn, AFF4_IMAGE_PERIOD,
       (RDFValue)this->image_period);

  CALL(oracle, resolve_value, this->urn, AFF4_TARGET_PERIOD,
       (RDFValue)this->target_period);

  CALL(oracle, resolve_value, this->urn, AFF4_SIZE,
       (RDFValue)((FileLikeObject)self)->size);

  CALL(segment,set, this->urn->value);
  CALL(segment, add, "map");

  // Now load our contents from the stored URN
  fd = (FileLikeObject)CALL(oracle, open, segment, 'r');
  if(!fd) goto error;
  else {
    char *map = talloc_strdup(self, CALL(fd,get_data));
    char *x=map, *y;
    struct map_point point;
    char target[1024];

    while(strlen(x)>0) {
      // Look for the end of line and null terminate it:
      y= x + strcspn(x, "\r\n");
      *y=0;
      if(sscanf(x,"%lld,%lld,%1000s", &point.image_offset,
                &point.target_offset, target)==3) {
        CALL(this, add_point, point.image_offset,
             point.target_offset, target);
      };
      x=y+1;
    };
  };

  CALL(oracle, cache_return, (AFFObject)fd);

  talloc_free(segment);
  return 1;
 error:
  talloc_free(segment);
  return 0;
};

static int MapValue_parse(RDFValue self, char *serialised) {
  return MapValue_decode(self, serialised, 0);
};

static int compare_points(const void *X, const void *Y) {
  struct map_point *x=(struct map_point *)X;
  struct map_point *y=(struct map_point *)Y;

  return x->image_offset - y->image_offset;
};

static char *MapValue_serialise(RDFValue self) {
  MapValue this = (MapValue)self;
  RDFURN stored = new_RDFURN(self);
  RDFURN segment = new_RDFURN(stored);
  int i;
  AFF4Volume volume;
  FileLikeObject fd;

  CALL(segment, set, this->urn->value);
  CALL(segment, add, "map");

  // Check if the map segment already exists - if it does we dont
  // bother re-saving it.
  fd = (FileLikeObject)CALL(oracle, open, segment, 'r');
  if(fd) {
    CALL((AFFObject)fd, cache_return);
    goto exit;
  };

  // Work out where our map object is stored:
  if(!CALL(oracle, resolve_value, this->urn, AFF4_STORED, (RDFValue)stored)) {
    RaiseError(ERuntimeError, "No storage for map object");
    goto error;
  };

  volume = (AFF4Volume)CALL(oracle, open, stored, 'w');
  if(!volume) goto error;

  // Is it actually a volume?
  if(!ISSUBCLASS(volume, AFF4Volume)) {
    RaiseError(ERuntimeError, "URN %s is not a volume!!!", STRING_URNOF(volume));
    CALL((AFFObject)volume, cache_return);
    goto error;
  };

  fd = CALL(volume, open_member, segment->value, 'w', ZIP_DEFLATE);
  // Done with our volume now
  CALL((AFFObject)volume, cache_return);

  if(!fd) {
    goto error;
  };

  // Now save the map to the segment
  // sort the array
  qsort(this->points, this->number_of_points, sizeof(*this->points),
	compare_points);

  for(i=0; i < this->number_of_points; i++) {
    char buff[BUFF_SIZE];
    struct map_point *point = &this->points[i];

    // See if we can reduce adjacent points
    // Note that due to the cache we can guarantee that if the
    // target_urn pointers are different, they are in fact different
    // URNs.
    if(i!=0 && i!=this->number_of_points &&
       point->target_index == this->points[i-1].target_index) {
      struct map_point *previous = &this->points[i-1];
      uint64_t prediction = point->image_offset - previous->image_offset + 
	previous->target_offset;

      // Dont write this point if its linearly related to previous point.
      if(prediction == point->target_offset) continue;
    };

    snprintf(buff, BUFF_SIZE, "%lld,%lld,%s\n", point->image_offset,
	     point->target_offset, this->targets[point->target_index]->value);
    CALL(fd, write, ZSTRING_NO_NULL(buff));
  };

  CALL(fd, close);

  // Now store various map parameters
  if(this->image_period->value != -1) {
    CALL(oracle, set_value, this->urn, AFF4_IMAGE_PERIOD, (RDFValue)this->image_period);
    CALL(oracle, set_value, this->urn, AFF4_TARGET_PERIOD, (RDFValue)this->target_period);
  };

  CALL(oracle, set_value, this->urn, AFF4_SIZE, (RDFValue)this->size);

 exit:
  return this->urn->value;

 error:
  if(fd)
    talloc_free(fd);
  talloc_free(stored);
  return NULL;
};

void MapValue_add_point(MapValue self, uint64_t image_offset, uint64_t target_offset,
                        char *target) {
  // Do we already have this target in the cache?
  XSDInteger target_index = (XSDInteger)CALL(self->cache, borrow, ZSTRING(target));
  struct map_point new_point;

  // Add another target to the array
  if(!target_index) {
    RDFURN target_urn = new_RDFURN(self);

    CALL(target_urn,set, target);

    target_index = new_XSDInteger(NULL);
    CALL(target_index, set, self->number_of_urns);

    self->targets = talloc_realloc_size(self, self->targets,
                                        (self->number_of_urns +1) *     \
                                        sizeof(*self->targets));
    self->targets[self->number_of_urns] = target_urn;

    self->number_of_urns ++ ;

    // Store it in the cache
    CALL(self->cache, put, ZSTRING(target), (Object)target_index);
    talloc_unlink(NULL, target_index);
  };

  new_point.target_offset = target_offset;
  new_point.image_offset = image_offset;
  new_point.target_index = target_index->value;

  // Now append the new point to our struct:
  self->points = talloc_realloc(self, self->points,
				struct map_point,
				(self->number_of_points+1) *    \
                                sizeof(*self->points));

  memcpy(&self->points[self->number_of_points], &new_point, sizeof(new_point));

  self->number_of_points ++;
};

// searches the array of map points and returns the offset in the
// array such that array[result].file_offset > offset
static int bisect_left(uint64_t offset, struct map_point *array, int hi) {
  uint64_t lo=0;
  uint64_t mid;

  while(lo < hi) {
    mid = (lo+hi)/2;
    if (array[mid].image_offset <= offset) {
      lo = mid+1;
    } else {
      hi = mid;
    };
  };
  return lo;
};

static int bisect_right(uint64_t offset, struct map_point *array, int hi) {
  uint64_t lo=0;
  uint64_t mid;

  while(lo < hi) {
    mid = (lo+hi)/2;
    if (offset < array[mid].image_offset) {
      hi = mid;
    } else {
      lo = mid+1;
    };
  };

  return lo-1;
};

static void MapValue_get_range(MapValue this, uint64_t readptr,
                     uint64_t *target_offset_at_point,
                     uint64_t *available_to_read,
                     RDFURN urn) {
  // How many periods we are from the start
  uint64_t period_number = readptr / (this->image_period->value);
  // How far into this period we are within the image
  uint64_t image_period_offset = readptr % (this->image_period->value);
  char direction = 'f';
  int l;

  *available_to_read = this->size->value - readptr;

  if(*available_to_read == 0) {
    *target_offset_at_point = 0;

    return;
  };

  // We can't interpolate forward before the first point - must
  // interpolate backwards.
  if(image_period_offset < this->points[0].image_offset) {
    direction = 'r';
  };

  /** Interpolate forward */
  if(direction=='f') {
    l = bisect_right(image_period_offset, this->points, this->number_of_points);

    // Here this->points[l].image_offset < image_period_offset
    *target_offset_at_point = this->points[l].target_offset  +   \
      image_period_offset - this->points[l].image_offset  +      \
      period_number * this->target_period->value;

    if(l < this->number_of_points-1) {
      *available_to_read = this->points[l+1].image_offset - \
        image_period_offset;
    } else {
      *available_to_read = min(*available_to_read, this->image_period->value -
                               image_period_offset);
    };

    /** Interpolate in reverse */
  } else {
    l = bisect_left(image_period_offset, this->points, this->number_of_points);
    *target_offset_at_point = this->points[l].target_offset - \
      (this->points[l].image_offset - image_period_offset) +    \
      period_number * this->target_period->value;

    if(l<this->number_of_points) {
      *available_to_read = this->points[l].image_offset - image_period_offset;
    } else {
      RaiseError(ERuntimeError, "Something went wrong...");
      return;
    };
  };

  if(urn && this->points[l].target_index < this->number_of_urns)
    CALL(urn, set, this->targets[this->points[l].target_index]->value);

  return;
};

VIRTUAL(MapValue, RDFValue) {
  VMETHOD_BASE(RDFValue, raptor_type) = RAPTOR_IDENTIFIER_TYPE_LITERAL;
  VMETHOD_BASE(RDFValue, dataType) = AFF4_MAP_TEXT;

  VMETHOD_BASE(RDFValue, Con) = MapValue_Con;
  VMETHOD_BASE(RDFValue, encode) = MapValue_encode;
  VMETHOD_BASE(RDFValue, decode) = MapValue_decode;
  VMETHOD_BASE(RDFValue, parse) = MapValue_parse;
  VMETHOD_BASE(RDFValue, serialise) = MapValue_serialise;
  VMETHOD_BASE(MapValue, add_point) = MapValue_add_point;
  VMETHOD_BASE(MapValue, get_range) = MapValue_get_range;
} END_VIRTUAL

/*** This is the implementation of the MapDriver */
static AFFObject MapDriver_Con(AFFObject self, RDFURN uri, char mode){ 
  MapDriver this = (MapDriver)self;

  // Try to parse existing map object
  if(uri) {
    URNOF(self) = CALL(uri, copy, self);

    this->stored = new_RDFURN(self);
    this->target_urn = new_RDFURN(self);

    // Check that we have a stored property
    if(!CALL(oracle, resolve_value, URNOF(self), AFF4_STORED,
	     (RDFValue)this->stored)) {
      RaiseError(ERuntimeError, "Map object does not have a stored attribute?");
      goto error;
    };

    CALL(oracle, set_value, URNOF(self), AFF4_TYPE, rdfvalue_from_string(self, AFF4_MAP));
    CALL(oracle, add_value, this->stored, AFF4_VOLATILE_CONTAINS, (RDFValue)uri);

    // Only update the timestamp if we created a new map
    if(mode=='w') {
      XSDDatetime time = new_XSDDateTime(this);
      XSDInteger i = new_XSDInteger(time);

      i->value = 1;

      CALL(oracle, set_value, URNOF(self), AFF4_TIMESTAMP, (RDFValue)time);
      // Make sure that our containing volume becomes dirty so we get
      // written to disk.
      CALL(oracle, set_value, this->stored, AFF4_VOLATILE_DIRTY, (RDFValue)i);

      // Make a new map object
      this->map = (MapValue)CALL(oracle, new_rdfvalue, self, AFF4_MAP_TEXT);

      // Make sure the map belongs to us
      CALL(this->map->urn, set, uri->value);
    } else {
      // We try to load the map from the resolver - because we dont
      // know the exact implementation we ask the resolver to allocate
      // it for us.
      this->map = (MapValue)CALL(oracle, resolve_alloc, self, uri, AFF4_MAP_DATA);

      if(!this->map) {
        RaiseError(EIOError, "Unable to open map object");
        goto error;
      };

      if(!ISSUBCLASS(this->map, MapValue)) {
        RaiseError(ERuntimeError, "Got unexpected value for map attribute '%s', needed %s",
                   ((RDFValue)this->map)->dataType, ((RDFValue)&__MapValue)->dataType);
        goto error;
      };
    };

    // We just share the map's size.
    ((FileLikeObject)self)->size = this->map->size;

    // Ignore it if we cant open the map_urn
    ClearError();
  };

  self = SUPER(AFFObject, FileLikeObject, Con, uri, mode);

  return self;

 error:
  talloc_free(self);
  return NULL;
};

static void MapDriver_add(MapDriver self, uint64_t image_offset, uint64_t target_offset,
			  char *target) {
  MapDriver this=self;

  CALL(this->map, add_point, image_offset, target_offset, target);

  if(self->super.size->value < image_offset) {
    self->super.size->value = image_offset;
  };
};

static void MapDriver_write_from(MapDriver self, RDFURN target, uint64_t target_offset, uint64_t target_length) {
  FileLikeObject this = (FileLikeObject)self;

  CALL(self->map, add_point, this->readptr, target_offset, target->value);
  this->readptr += target_length;
  this->size->value = max(this->size->value, this->readptr);
};


static void MapDriver_close(FileLikeObject self) {
  MapDriver this = (MapDriver)self;

  // Write the map to the stream:
  CALL(oracle, set_value, URNOF(self), AFF4_MAP_DATA, (RDFValue)this->map);

  // Done
  talloc_free(self);
};

// Read as much as possible and return how much was read
static int MapDriver_partial_read(FileLikeObject self, char *buffer, \
				  unsigned long int length) {
  MapDriver this = (MapDriver)self;
  FileLikeObject target;
  int read_bytes;
  uint64_t target_offset_at_point, available_to_read;

  CALL(this->map, get_range, self->readptr, &target_offset_at_point, &available_to_read,
       this->target_urn);

  // Clamp the available_to_read to the length requested
  available_to_read = min(available_to_read, length);

  // Now do the read:
  target = (FileLikeObject)CALL(oracle, open,  this->target_urn, 'r');
  if(!target) return -1;

  CALL(target, seek, target_offset_at_point, SEEK_SET);
  read_bytes = CALL(target, read, buffer, available_to_read);

  CALL(oracle, cache_return, (AFFObject)target);

  if(read_bytes >0) {
    ((FileLikeObject)self)->readptr += read_bytes;
  };

  return read_bytes;
};

static int MapDriver_read(FileLikeObject self, char *buffer, unsigned long int length) {
  int i=0;
  int read_length;

  // Clip the read to the stream size
  length = min(length, self->size->value - self->readptr);

  // Just execute as many partial reads as are needed to satisfy the
  // length requested
  while(i < length ) {
    read_length = MapDriver_partial_read(self, buffer + i, length - i);
    if(read_length==0) break;
    if(read_length < 0) {
      // An error occurred - we need to decide if to pad or not
      /*
      char *pad = (char *)CALL(oracle, resolve, self, 
			       CONFIGURATION_NS, CONFIG_PAD,
			       RESOLVER_DATA_STRING);
      */
      int pad = 1;

      if(pad) {
	memset(buffer + i ,0, length -i);
      } else {
	PrintError();
	return -1;
      };
    };
    i += read_length;
  };

  return i;
};


VIRTUAL(MapDriver, FileLikeObject) {
     VMETHOD_BASE(AFFObject, Con) = MapDriver_Con;
     VMETHOD_BASE(AFFObject, dataType) = AFF4_MAP;

     VMETHOD(add) = MapDriver_add;
     VMETHOD(write_from) = MapDriver_write_from;
     VMETHOD_BASE(FileLikeObject, read) = MapDriver_read;
     VMETHOD_BASE(FileLikeObject, close) = MapDriver_close;

     // Cant directly write to maps
     UNIMPLEMENTED(FileLikeObject, write);
} END_VIRTUAL

void mapdriver_init() {
  INIT_CLASS(MapDriver);
  INIT_CLASS(MapValue);

  register_type_dispatcher(AFF4_MAP, (AFFObject *)GETCLASS(MapDriver));
  register_rdf_value_class((RDFValue)GETCLASS(MapValue));
}
