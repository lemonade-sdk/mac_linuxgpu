/* Exercises the production IRQ table and DriverKit-context TLS with host threads. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <linux/interrupt.h>
#include <rt/rt.h>
#include <rt/dext_pci.h>

extern int linuxu_rt_inject_irq(int);
#ifdef LINUXU_DEXT_DK
int dext_pci_irq_status(unsigned int *armed, unsigned int *type)
{
    *armed = 16; *type = DEXT_PCI_IRQ_MSIX; return 0;
}
int IOThreadLocalStorageKeyCreate(uint64_t *key)
{
    pthread_key_t native;
    int r = pthread_key_create(&native, NULL);
    if (!r) *key = native;
    return r;
}
int IOThreadLocalStorageSet(uint64_t key, const void *value)
{
    return pthread_setspecific((pthread_key_t)key, value);
}
void *IOThreadLocalStorageGet(uint64_t key)
{
    return pthread_getspecific((pthread_key_t)key);
}
#endif

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int entered, allowed, hits;
static _Atomic(int) barrier_started, barrier_done;
static int cookie;
static irqreturn_t handler(int vector, void *arg)
{
    assert(vector == 7 && arg == &cookie && in_irq());
    pthread_mutex_lock(&gate);
    hits++; entered = 1;
    pthread_cond_broadcast(&changed);
    while (!allowed) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    assert(in_irq());
    return IRQ_HANDLED;
}
static void *inject(void *unused)
{
    (void)unused;
    assert(!in_irq());
    assert(linuxu_rt_inject_irq(7) == IRQ_HANDLED);
    assert(!in_irq());
    return NULL;
}
static void wait_entered(void)
{
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
static void release_handler(void)
{
    pthread_mutex_lock(&gate);
    allowed = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void *synchronize(void *unused)
{
    (void)unused;
    atomic_store(&barrier_started, 1);
    synchronize_irq(7);
    atomic_store(&barrier_done, 1);
    return NULL;
}
static void *disable(void *unused)
{
    (void)unused;
    atomic_store(&barrier_started, 1);
    assert(disable_irq(7) == 0);
    atomic_store(&barrier_done, 1);
    return NULL;
}
static void *unregister(void *unused)
{
    (void)unused;
    atomic_store(&barrier_started, 1);
    free_irq(7, &cookie);
    atomic_store(&barrier_done, 1);
    return NULL;
}
static void run_barrier(void *(*barrier)(void *))
{
    pthread_t irq, waiter;
    entered = allowed = 0;
    atomic_store(&barrier_started, 0); atomic_store(&barrier_done, 0);
    assert(pthread_create(&irq, NULL, inject, NULL) == 0);
    wait_entered();
    assert(!in_irq()); /* a concurrent IRQ must not change this thread's context */
    assert(pthread_create(&waiter, NULL, barrier, NULL) == 0);
    while (!atomic_load(&barrier_started)) usleep(100);
    usleep(20000);
    assert(!atomic_load(&barrier_done));
    assert(rt_irq_register(NULL, 7, (rt_irq_handler_t)handler, "busy", &cookie) != 0);
    release_handler();
    assert(pthread_join(irq, NULL) == 0);
    assert(pthread_join(waiter, NULL) == 0);
    assert(atomic_load(&barrier_done));
}
static irqreturn_t disable_from_handler(int vector, void *arg)
{
    (void)arg;
    assert(in_irq());
    assert(disable_irq_nosync(vector) == 0);
    return IRQ_HANDLED;
}
int main(void)
{
    alarm(10); /* turn a disable/drain deadlock into a bounded test failure */
    assert(rt_irq_register(NULL, 7, NULL, "null", &cookie) != 0);
    assert(request_irq(16, handler, 0, "unarmed", &cookie) < 0);
    assert(request_threaded_irq(7, handler, handler, 0, "threaded", &cookie) < 0);
    assert(request_irq(7, handler, 0, "lifetime", &cookie) == 0);
    run_barrier(synchronize);
    run_barrier(disable);
    int before = hits;
    assert(linuxu_rt_inject_irq(7) == 0 && hits == before);
    assert(disable_irq_nosync(7) == 0); /* disable nesting */
    enable_irq(7);
    assert(linuxu_rt_inject_irq(7) == 0 && hits == before);
    enable_irq(7);
    assert(linuxu_rt_inject_irq(7) == IRQ_HANDLED);
    free_irq(7, (void *)(uintptr_t)1); /* another client's cookie cannot remove it */
    assert(linuxu_rt_inject_irq(7) == IRQ_HANDLED);
    run_barrier(unregister);
    assert(linuxu_rt_inject_irq(7) == 0);
    assert(request_irq(7, handler, 0, "reuse", &cookie) == 0);
    free_irq(7, &cookie);
    assert(request_irq(8, disable_from_handler, 0, "self-mask", &cookie) == 0);
    assert(linuxu_rt_inject_irq(8) == IRQ_HANDLED);
    assert(linuxu_rt_inject_irq(8) == 0);
    free_irq(8, &cookie);
    assert(disable_irq(256) < 0);
    alarm(0);
    return 0;
}
