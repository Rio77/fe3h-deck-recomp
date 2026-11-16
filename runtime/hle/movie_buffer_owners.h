// Host-only ownership; the guest Buffer ABI is unchanged. Caller serializes access.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
typedef struct MovieAllocation { void *base; size_t size, refs; } MovieAllocation;
typedef struct MovieBufferRef { uint64_t guest; MovieAllocation *allocation; struct MovieBufferRef *next; } MovieBufferRef;
typedef struct { MovieBufferRef *head; size_t bytes, created, freed; } MovieBufferOwners;
static inline MovieAllocation *movie_owned_find(MovieBufferOwners *s, uint64_t guest) {
    for (MovieBufferRef *r=s->head;r;r=r->next) if (r->guest==guest) return r->allocation;
    return NULL;
}
static inline void movie_owned_release(MovieBufferOwners *s, uint64_t guest) {
    MovieBufferRef **p=&s->head;
    while (*p && (*p)->guest!=guest) p=&(*p)->next;
    if (!*p) return;
    MovieBufferRef *r=*p;*p=r->next;MovieAllocation *a=r->allocation;
    if (!--a->refs) { s->bytes-=a->size;s->freed++;free(a->base);free(a); }
    free(r);
}
static inline bool movie_owned_add(MovieBufferOwners *s,uint64_t guest,void *base,size_t size) {
    MovieAllocation *a=malloc(sizeof *a);MovieBufferRef *r=malloc(sizeof *r);
    if (!a || !r) { free(a);free(r);return false; }
    movie_owned_release(s,guest);
    *a=(MovieAllocation){base,size,1};*r=(MovieBufferRef){guest,a,s->head};s->head=r;
    s->bytes+=size;s->created++;return true;
}
static inline bool movie_owned_copy(MovieBufferOwners *s,uint64_t dst,uint64_t src) {
    if (dst==src) return true;
    MovieAllocation *a=movie_owned_find(s,src);
    MovieBufferRef *r=a?malloc(sizeof *r):NULL;
    if (a && !r) return false;
    movie_owned_release(s,dst);
    if (a) { a->refs++;*r=(MovieBufferRef){dst,a,s->head};s->head=r; }
    return true;
}
static inline void movie_owned_reset(MovieBufferOwners *s) {
    while (s->head) movie_owned_release(s,s->head->guest);
}
