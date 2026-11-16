#include <assert.h>
#include <stdio.h>
#include "../runtime/services/am_storage_owners.h"
static NativeOwners owner={.lock=PTHREAD_MUTEX_INITIALIZER};
static void *share(void *arg) {
    AmStorage *s=arg;
    for(int i=0;i<10000;i++){am_storage_retain_owned(s);assert(s->data[0]==123);am_storage_release_owned(s);}
    return NULL;
}
int main(void) {
    for(int i=0;i<1000;i++) {
        AmStorage *parent=am_storage_create_owned(&owner,136);assert(parent && parent->size==136 && !parent->data[0]);
        parent->data[0]=123;am_storage_retain_owned(parent);AmStorage *accessor=parent;
        am_storage_release_owned(parent);assert(owner.count==1 && accessor->data[0]==123);
        am_storage_release_owned(accessor);assert(!owner.count && !owner.bytes);
    }
    AmStorage *s=am_storage_create_owned(&owner,136);assert(s);s->data[0]=123;
    pthread_t threads[4];for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,share,s));
    for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(owner.count==1 && atomic_load(&s->refs)==1);am_storage_release_owned(s);
    assert(!owner.count && !owner.bytes && owner.created==owner.freed);
    assert(!am_storage_create_owned(&owner,UINT64_MAX));
    puts("PASS launch storage ownership: 1000 parent-first closes, accessor data retained, 40000 concurrent shares, all bodies freed, overflow rejected");
}
