static inline OBJECT _om_inline_new_object(object_memory om, OBJECT cls, int fields) {
  int size, i, f;
  OBJECT obj, flags;
  struct rubinius_object *header;
    
  //fields += 4; /* PAD */
  size = (HEADER_SIZE + fields) * REFSIZE;
  if(!heap_enough_space_p(om->gc->current, size)) {
    obj = (OBJECT)baker_gc_allocate_spilled(om->gc, size);
    assert(heap_enough_space_p(om->gc->next, size));
    // DEBUG("Ran out of space! spilled into %p\n", obj);
    om->collect_now |= 1;
    // baker_gc_enlarge_next(om->gc, om->gc->current->size * GC_SCALING_FACTOR);
  } else {
    obj = (OBJECT)baker_gc_allocate(om->gc, size);
  }
  
  header = (struct rubinius_object*)obj;
  header->klass = cls;
  SET_NUM_FIELDS(obj, fields);
  if(cls && REFERENCE_P(cls)) {
    /* #define CLASS_f_INSTANCE_FLAGS 8 */
    flags = NTH_FIELD(cls, 8);
    f = FIXNUM_TO_INT(flags);
    header->flags = f;
  } else {
    header->flags = 0;
  }
  header->flags2 = 0;
  for(i = 0; i < fields; i++) {
    rbs_set_field(om, obj, i, Qnil);
  }
  
  GC_ZONE_SET(obj, GC_YOUNG_OBJECTS);
  
  /* The hash is unique by default, just an auto incremented ID */
  header->hash = om->last_object_id++;
  return obj;
}
