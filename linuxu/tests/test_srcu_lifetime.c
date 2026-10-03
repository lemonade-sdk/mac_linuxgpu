/* Old readers delay teardown, while new readers and unrelated domains do not. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/srcu.h>
#include "../src/rcu.c"

static DEFINE_SRCU(domain);
static DEFINE_SRCU(other_domain);
static atomic_int synchronized;
static void *synchronize_one(void *unused)
{
    (void)unused;
    synchronize_srcu(&domain);
    atomic_store(&synchronized, 1);
    return NULL;
}
static void wait_epoch(unsigned int epoch)
{
    for (;;) {
        pthread_mutex_lock(&srcu_lock);
        bool changed = domain.epoch == epoch && domain.grace_period_active;
        pthread_mutex_unlock(&srcu_lock);
        if (changed) return;
        usleep(100);
    }
}
int main(void)
{
    alarm(10);
    pthread_t worker;
    int old = srcu_read_lock(&domain);
    int nested = srcu_read_lock(&domain);
    assert(old == nested && old == 0);
    assert(pthread_create(&worker, NULL, synchronize_one, NULL) == 0);
    wait_epoch(1);
    assert(!atomic_load(&synchronized));
    int recent = srcu_read_lock(&domain);
    assert(recent == 1);
    /* Waiting for another domain while holding this reader must work. */
    synchronize_srcu(&other_domain);
    srcu_read_unlock(&domain, old);
    assert(!atomic_load(&synchronized));
    srcu_read_unlock(&domain, nested);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&synchronized));
    /* A subsequent teardown must include the reader of the newer epoch. */
    atomic_store(&synchronized, 0);
    assert(pthread_create(&worker, NULL, synchronize_one, NULL) == 0);
    wait_epoch(0);
    assert(!atomic_load(&synchronized));
    srcu_read_unlock(&domain, recent);
    assert(pthread_join(worker, NULL) == 0);
    cleanup_srcu_struct(&domain);
    assert(!init_srcu_struct(&domain));
    old = srcu_read_lock(&domain);
    srcu_read_unlock(&domain, old);
    synchronize_srcu_expedited(&domain);
    puts("SRCU teardown, nested readers, epochs and independent domains passed");
}
