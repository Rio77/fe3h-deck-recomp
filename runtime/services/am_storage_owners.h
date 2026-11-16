// Host launch-parameter storage shared by IStorage and its accessors.
#pragma once
#include <stdatomic.h>
#include "../native_owners.h"
typedef struct {
    uint8_t *data;
    uint64_t size;
    atomic_uint refs;
    NativeOwners *owner;
} AmStorage;
static inline AmStorage *am_storage_create_owned(NativeOwners *owner,uint64_t size) {
    if(size>SIZE_MAX-sizeof(AmStorage))return NULL;
    AmStorage *s=native_owned_calloc(owner,1,sizeof *s+(size_t)size);
    if(!s)return NULL;
    s->data=(uint8_t *)(s+1);s->size=size;s->owner=owner;atomic_init(&s->refs,1);
    return s;
}
static inline void am_storage_retain_owned(AmStorage *s) {
    atomic_fetch_add_explicit(&s->refs,1,memory_order_relaxed);
}
static inline void am_storage_release_owned(AmStorage *s) {
    if(s && atomic_fetch_sub_explicit(&s->refs,1,memory_order_acq_rel)==1)
        native_owned_free(s->owner,s);
}
