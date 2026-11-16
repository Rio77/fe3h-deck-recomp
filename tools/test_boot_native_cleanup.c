#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/kernel.c"
#include "../runtime/ipc.c"

volatile int g_rt_unwind;
void rt_fatal_trace(void) { abort(); }
void plat_crash_stack_stats(uint64_t *created,uint64_t *freed) { *created=*freed=0; }
const char *plat_symbol(uint64_t address,uint64_t *offset) { (void)address;(void)offset;return NULL; }

static unsigned closed;
static int borrowed=42;
static void close_owned(Service *s) {
    assert(s->state);free(s->state);s->state=NULL;closed++;
}
static void handler(Service *s,IpcReq *req,IpcResp *resp) { (void)s;(void)req;(void)resp; }
typedef struct { unsigned magic; } Parent;
typedef struct { Parent *parent; } Child;
static void close_child(Service *s) {
    Child *c=s->state;assert(c->parent->magic==0x12345678);free(c);s->state=NULL;closed++;
}
static KEvent *persistent;
static _Atomic bool stop_timer;
static _Atomic unsigned signals;
static void *timer(void *unused) {
    (void)unused;
    while(!atomic_load(&stop_timer)) {
        pthread_mutex_lock(&g_klock);kernel_signal(persistent);pthread_mutex_unlock(&g_klock);
        atomic_fetch_add(&signals,1);usleep(100);
    }
    return NULL;
}
int main(void) {
    uint32_t pw,pr;persistent=kernel_create_persistent_event_pair(&pw,&pr);
    pthread_t thread;assert(!pthread_create(&thread,NULL,timer,NULL));
    for(unsigned i=0;i<1000 && !atomic_load(&signals);i++)usleep(100);
    assert(signals>0);
    for(unsigned boot=0;boot<100;boot++) {
        for(unsigned i=0;i<30;i++) {
            uint32_t w,r;KEvent *event=kernel_create_event_pair(&w,&r);
            assert(event!=persistent && handle_get(w)==event && handle_get(r)==event);
            handle_close(w);handle_close(r);
            Service *s=service_new("test-owned",handler,calloc(1,32));s->on_close=close_owned;
            session_new(s);session_new(s);
            if(i%2==0){service_release(s);service_release(s);}
        }
        Parent *parent=calloc(1,sizeof *parent);parent->magic=0x12345678;
        // Deliberately put the parent first in the registry, proving teardown
        // does not free its state before the later child close callback.
        Child *child=calloc(1,sizeof *child);child->parent=parent;
        Service *cs=service_new("child",handler,child);cs->on_close=close_child;
        Service *ps=service_new("parent",handler,parent);ps->owns_state=true;
        Service *bs=service_new("borrowed",handler,&borrowed);assert(!bs->owns_state);
        kernel_host_calloc(1,sizeof(Domain));
        assert(kernel_owners.count && service_owners.count);
        ipc_reset();kernel_reset();
        assert(!kernel_owners.count && !kernel_owners.bytes && kernel_owners.created==kernel_owners.freed);
        assert(!service_owners.count && !service_owners.bytes && service_owners.created==service_owners.freed);
        assert(!owned_services && borrowed==42 && closed==(boot+1)*31);
        assert(persistent->type==KOBJ_EVENT && persistent->refs==0);
        assert(!handle_get(0x101));
        pw=kernel_new_handle(persistent);pr=kernel_new_handle(persistent);assert(pw!=pr);
    }
    atomic_store(&stop_timer,true);assert(!pthread_join(thread,NULL));assert(signals>0);
    handle_close(pw);handle_close(pr);free(persistent);
    puts("PASS 100 native boot cleanups: shared timer survives, child callbacks precede parent release, borrowed state preserved, all retired ownership balanced");
}
