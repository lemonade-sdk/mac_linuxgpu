/* Offline reservation contention and two-lock deadlock recovery. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/errno.h>
#include <linux/ww_mutex.h>

void msleep(unsigned int ms) { usleep(ms * 1000); }
static DEFINE_WW_CLASS(test_class);
static struct ww_mutex a, b;
static struct ww_acquire_ctx old, young;
static unsigned entered, finished;
static unsigned broadcasts;

int ww_test_broadcast(pthread_cond_t *condition)
{
	__atomic_add_fetch(&broadcasts, 1, __ATOMIC_RELAXED);
	return pthread_cond_broadcast(condition);
}

static void *older_waiter(void *opaque)
{
	__atomic_add_fetch(&entered, 1, __ATOMIC_RELEASE);
	assert(ww_mutex_lock(&a, opaque) == 0);
	ww_mutex_unlock(&a);
	return NULL;
}

static void wound_once(void)
{
	struct ww_acquire_ctx first, second, owner;
	pthread_t waiters[2];
	test_class.is_wait_die = 0;
	ww_acquire_init(&first, &test_class);
	ww_acquire_init(&second, &test_class);
	ww_acquire_init(&owner, &test_class);
	ww_mutex_init(&a, &test_class);
	assert(ww_mutex_lock(&a, &owner) == 0);
	entered = broadcasts = 0;
	assert(!pthread_create(&waiters[0], NULL, older_waiter, &first));
	assert(!pthread_create(&waiters[1], NULL, older_waiter, &second));
	while (__atomic_load_n(&entered, __ATOMIC_ACQUIRE) != 2) usleep(100);
	while (!__atomic_load_n(&owner.wounded, __ATOMIC_RELAXED)) usleep(100);
	usleep(20000);
	/* A second waiter must not keep waking the first without state change. */
	assert(__atomic_load_n(&broadcasts, __ATOMIC_RELAXED) == 1);
	ww_mutex_unlock(&a);
	assert(!pthread_join(waiters[0], NULL));
	assert(!pthread_join(waiters[1], NULL));
	assert(!first.acquired && !second.acquired && !owner.acquired);
	ww_mutex_destroy(&a);
}
static struct mutex final_lock;
static atomic_t users;

static void *last_user(void *unused)
{
	(void)unused;
	__atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
	/* A new user was acquired while the final decrement waited for the lock. */
	assert(!atomic_dec_and_mutex_lock(&users, &final_lock));
	return NULL;
}

static void final_reference(void)
{
	pthread_t thread;
	mutex_init(&final_lock);
	atomic_set(&users, 2);
	assert(!atomic_dec_and_mutex_lock(&users, &final_lock));
	assert(atomic_read(&users) == 1 && !mutex_is_locked(&final_lock));
	assert(atomic_dec_and_mutex_lock(&users, &final_lock));
	assert(atomic_read(&users) == 0 && mutex_is_locked(&final_lock));
	mutex_unlock(&final_lock);
	atomic_set(&users, 1);
	mutex_lock(&final_lock);
	entered = 0;
	assert(pthread_create(&thread, NULL, last_user, NULL) == 0);
	while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE)) usleep(100);
	atomic_inc(&users);
	mutex_unlock(&final_lock);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_read(&users) == 1 && !mutex_is_locked(&final_lock));
	mutex_destroy(&final_lock);
}

static void *lock_second(void *unused)
{
	(void)unused;
	__atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
	assert(ww_mutex_lock(&b, &old) == 0);
	assert(old.acquired == 2);
	ww_mutex_unlock(&b);
	ww_mutex_unlock(&a);
	__atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void opposite_order(unsigned wait_die)
{
	pthread_t thread;
	test_class.is_wait_die = wait_die;
	ww_acquire_init(&old, &test_class);
	ww_acquire_init(&young, &test_class);
	assert(young.stamp.counter != old.stamp.counter);
	ww_mutex_init(&a, &test_class);
	ww_mutex_init(&b, &test_class);
	assert(ww_mutex_lock(&a, &old) == 0);
	assert(ww_mutex_lock(&a, &old) == -EALREADY);
	assert(old.acquired == 1);
	assert(ww_mutex_trylock(&b, &young) == 1);
	assert(young.acquired == 1);
	assert(ww_mutex_trylock(&a, &young) == 0);
	entered = finished = 0;
	assert(pthread_create(&thread, NULL, lock_second, NULL) == 0);
	while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE)) usleep(100);
	if (!wait_die) {
		while (!__atomic_load_n(&young.wounded, __ATOMIC_RELAXED)) usleep(100);
		ww_acquire_done(&young);
		assert(young.wounded); /* completing acquisition must not lose a wound */
	}
	assert(ww_mutex_lock(&a, &young) == -EDEADLK);
	assert(!__atomic_load_n(&finished, __ATOMIC_ACQUIRE));
	ww_mutex_unlock(&b);
	assert(young.acquired == 0);
	ww_mutex_lock_slow(&a, &young);
	assert(young.acquired == 1);
	assert(!young.wounded);
	ww_mutex_unlock(&a);
	assert(pthread_join(thread, NULL) == 0);
	assert(!old.acquired && !young.acquired);
	ww_acquire_fini(&young);
	ww_acquire_fini(&old);
	ww_mutex_destroy(&a);
	ww_mutex_destroy(&b);
}

static void *anonymous_waiter(void *unused)
{
	(void)unused;
	__atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
	assert(ww_mutex_lock(&a, NULL) == 0);
	__atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
	ww_mutex_unlock(&a);
	return NULL;
}

int main(void)
{
	pthread_t thread;
	alarm(15);
	final_reference();
	wound_once();
	for (unsigned i = 0; i < 64; ++i) {
		opposite_order(0);
		opposite_order(1);
	}
	ww_mutex_init(&a, &test_class);
	assert(ww_mutex_lock(&a, NULL) == 0);
	entered = finished = 0;
	assert(pthread_create(&thread, NULL, anonymous_waiter, NULL) == 0);
	while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE)) usleep(100);
	usleep(2000);
	assert(!__atomic_load_n(&finished, __ATOMIC_ACQUIRE));
	ww_mutex_unlock(&a);
	assert(pthread_join(thread, NULL) == 0);
	assert(finished);
	ww_mutex_destroy(&a);
	puts("reservation contention, ownership and deadlock recovery passed");
	return 0;
}
