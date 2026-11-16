#include <assert.h>
#include <stdio.h>
#include "../runtime/hle/movie_buffer_owners.h"
int main(void) {
    MovieBufferOwners s={0};
    for (int i=0;i<1000;i++) {
        unsigned char *a=malloc(256);a[0]=73;
        assert(movie_owned_add(&s,1,a,256));
        assert(movie_owned_copy(&s,2,1));assert(movie_owned_copy(&s,3,2));
        assert(movie_owned_copy(&s,1,1));
        movie_owned_release(&s,1);assert(((unsigned char*)movie_owned_find(&s,3)->base)[0]==73);
        // Assignment from a borrowed object releases only the destination's owner.
        assert(movie_owned_copy(&s,2,999));assert(!movie_owned_find(&s,2));assert(s.bytes==256);
        // Reconstruct a destination that previously shared the same allocation.
        void *b=malloc(32);assert(movie_owned_add(&s,2,b,32));assert(s.bytes==288);
        assert(movie_owned_copy(&s,2,3));assert(s.bytes==256);
        movie_owned_release(&s,3);assert(s.bytes==256);
        movie_owned_reset(&s);assert(!s.bytes && !s.head && s.created==s.freed);
        movie_owned_release(&s,2); // repeated destruction is safe
    }
    puts("movie Buffer ownership: 1000 cycles, shared copies, assignment, borrowed buffers, reconstruction and reset passed");
}
