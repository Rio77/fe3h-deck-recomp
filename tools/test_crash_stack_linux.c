#define _GNU_SOURCE
#include <assert.h>
#include "../runtime/platform/linux.c"
bool rt_fault_watch(uint64_t a,uint64_t b) { (void)a;(void)b;return false; }
void rt_fault_report(const char *w,uint64_t a,uint64_t b,uint64_t c) { (void)w;(void)a;(void)b;(void)c;_Exit(99); }
static __thread volatile sig_atomic_t signal_on_alternate;
static void test_signal(int sig) {
    (void)sig;char local;
    stack_t ss;if (!sigaltstack(NULL,&ss))
        signal_on_alternate=(uintptr_t)&local >= (uintptr_t)ss.ss_sp && (uintptr_t)&local < (uintptr_t)ss.ss_sp+ss.ss_size;
}
static void *worker(void *arg) {
    plat_crash_handler_install();void *first=pthread_getspecific(crash_stack_key);assert(first);
    plat_crash_handler_install();assert(pthread_getspecific(crash_stack_key)==first);
    raise(SIGUSR1);assert(signal_on_alternate);
    if (arg) pthread_exit(NULL);
    return NULL;
}
static void *borrowed(void *arg) {
    (void)arg;void *mem=malloc(1u<<20);assert(mem);
    stack_t ss={.ss_sp=mem,.ss_size=1u<<20};assert(!sigaltstack(&ss,NULL));
    plat_crash_handler_install();assert(!pthread_getspecific(crash_stack_key));
    stack_t current;assert(!sigaltstack(NULL,&current) && current.ss_sp==mem);
    stack_t off={.ss_flags=SS_DISABLE};assert(!sigaltstack(&off,NULL));free(mem);return NULL;
}
int main(void) {
    struct sigaction sa={0};sa.sa_handler=test_signal;sa.sa_flags=SA_ONSTACK;sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGUSR1,&sa,NULL));
    for (int round=0;round<100;round++) {
        pthread_t threads[8];for (int i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)(i&1)));
        for (int i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    }
    pthread_t thread;assert(!pthread_create(&thread,NULL,borrowed,NULL));assert(!pthread_join(thread,NULL));
    uint64_t created,freed;plat_crash_stack_stats(&created,&freed);
    assert(created==800 && freed==created);
    puts("PASS: 800 threads, repeat install, normal exit, pthread_exit, alternate-stack signal, borrowed stack, balanced allocations");
}
