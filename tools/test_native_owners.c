#include <assert.h>
#include <stdio.h>
#include "../runtime/native_owners.h"
static NativeOwners r={.lock=PTHREAD_MUTEX_INITIALIZER};
static void *worker(void *unused) {
    (void)unused;
    for (int i=0;i<1000;i++) {
        long double *p=native_owned_calloc(&r,3,sizeof *p);assert(p && p[0]==0 && (uintptr_t)p%_Alignof(max_align_t)==0);
        p[0]=1.25L;assert(native_owned_free(&r,p));
        assert(native_owned_calloc(&r,1,17)); // intentionally retained until reset
    }
    return NULL;
}
int main(void) {
    pthread_t threads[8];for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for (int i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    assert(r.count==8000 && r.bytes==8000*17);
    native_owned_reset(&r);assert(!r.count && !r.bytes && r.created==r.freed);
    assert(!native_owned_calloc(&r,SIZE_MAX,2));assert(native_owned_free(&r,NULL));
    native_owned_reset(&r);puts("PASS native ownership: alignment, zeroing, 8 concurrent producers, 16000 allocations, retirement and overflow");
}
