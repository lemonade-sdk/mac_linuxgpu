/* Actual devres list/callback ownership under competing add/release threads. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <linux/device.h>
#include <linux/slab.h>

void *kmalloc(size_t n, gfp_t flags) { return flags & __GFP_ZERO ? calloc(1,n) : malloc(n); }
void *kzalloc(size_t n, gfp_t flags) { (void)flags; return calloc(1,n); }
void kfree(const void *p) { free((void *)p); }
static struct device dev;
static atomic_uint released, writers_done;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int callback_entered, callback_continue;
static atomic_bool second_entered, second_done;

static void release_allocation(void *p) { free(p); __c11_atomic_fetch_add(&released, 1, __ATOMIC_RELAXED); }
static void *writer(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 256; ++i) {
        void *p = malloc(19); assert(p);
        /* Failure runs the cleanup synchronously; every allocation has one owner. */
        (void)devm_add_action_or_reset(&dev, release_allocation, p);
    }
    __c11_atomic_fetch_add(&writers_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void *releaser(void *unused)
{
    (void)unused;
    while (atomic_load(&writers_done) != 8) devres_release_all(&dev);
    return NULL;
}
static void blocking_callback(void *unused)
{
    (void)unused;
    devres_release_all(&dev); /* same-thread recursion must be harmless */
    void *p = malloc(19);
    assert(devm_add_action_or_reset(&dev, release_allocation, p) < 0);
    pthread_mutex_lock(&lock);
    callback_entered = 1;
    pthread_cond_broadcast(&ready);
    while (!callback_continue) pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
}
static void *first_release(void *unused) { (void)unused; devres_release_all(&dev); return NULL; }
static void *second_release(void *unused)
{
    (void)unused; atomic_store(&second_entered, true);
    devres_release_all(&dev); atomic_store(&second_done, true); return NULL;
}
int main(void)
{
    pthread_t writers[8], cleanup;
    assert(!pthread_create(&cleanup, NULL, releaser, NULL));
    for (unsigned i=0; i<8; ++i) assert(!pthread_create(&writers[i],NULL,writer,NULL));
    for (unsigned i=0; i<8; ++i) assert(!pthread_join(writers[i],NULL));
    assert(!pthread_join(cleanup,NULL));
    devres_release_all(&dev);
    assert(atomic_load(&released) == 8 * 256 && !dev.devres);

    assert(!devm_add_action(&dev, blocking_callback, NULL));
    pthread_t first, second;
    assert(!pthread_create(&first,NULL,first_release,NULL));
    pthread_mutex_lock(&lock);
    while (!callback_entered) pthread_cond_wait(&ready,&lock);
    pthread_mutex_unlock(&lock);
    assert(!pthread_create(&second,NULL,second_release,NULL));
    while (!atomic_load(&second_entered)) sched_yield();
    struct timespec brief = { .tv_nsec = 20000000 };
    nanosleep(&brief,NULL);
    assert(!atomic_load(&second_done));
    pthread_mutex_lock(&lock); callback_continue=1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&lock);
    assert(!pthread_join(first,NULL) && !pthread_join(second,NULL));
    assert(atomic_load(&second_done) && atomic_load(&released) == 8 * 256 + 1);
    return 0;
}
