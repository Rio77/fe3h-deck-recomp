// Native allocations owned by one guest boot. No guest data or backend caches.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
typedef struct NativeOwners NativeOwners;
typedef union NativeHeader NativeHeader;
union NativeHeader {
    struct { NativeHeader *prev,*next;NativeOwners *owner;size_t size; } data;
    max_align_t alignment;
};
struct NativeOwners {
    pthread_mutex_t lock;
    NativeHeader *head;
    size_t count,bytes,created,freed;
};
static inline void *native_owned_calloc(NativeOwners *r,size_t n,size_t size) {
    if (n && size>(SIZE_MAX-sizeof(NativeHeader))/n) return NULL;
    size_t bytes=n*size;
    NativeHeader *h=calloc(1,sizeof *h+bytes);if (!h) return NULL;
    h->data.owner=r;h->data.size=bytes;
    pthread_mutex_lock(&r->lock);h->data.next=r->head;
    if (r->head) r->head->data.prev=h;
    r->head=h;r->count++;r->bytes+=bytes;r->created++;
    pthread_mutex_unlock(&r->lock);return h+1;
}
static inline bool native_owned_free(NativeOwners *r,void *p) {
    if (!p) return true;
    NativeHeader *h=(NativeHeader*)p-1;
    if (h->data.owner!=r) return false;
    pthread_mutex_lock(&r->lock);
    if (h->data.prev) h->data.prev->data.next=h->data.next;else r->head=h->data.next;
    if (h->data.next) h->data.next->data.prev=h->data.prev;
    r->count--;r->bytes-=h->data.size;r->freed++;h->data.owner=NULL;
    pthread_mutex_unlock(&r->lock);free(h);return true;
}
static inline void native_owned_reset(NativeOwners *r) {
    // All producers must have stopped. Destructor intentionally calls no services.
    pthread_mutex_lock(&r->lock);
    while (r->head) {
        NativeHeader *h=r->head;r->head=h->data.next;
        r->count--;r->bytes-=h->data.size;r->freed++;free(h);
    }
    pthread_mutex_unlock(&r->lock);
}
