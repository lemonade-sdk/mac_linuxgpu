/* Host mock for the DriverKit IOLock-backed pthread ABI adapters. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

#define DK(name) linuxu_dk_##name
extern int DK(pthread_mutex_init)(pthread_mutex_t *, const pthread_mutexattr_t *);
extern int DK(pthread_mutex_destroy)(pthread_mutex_t *);
extern int DK(pthread_mutex_lock)(pthread_mutex_t *);
extern int DK(pthread_mutex_trylock)(pthread_mutex_t *);
extern int DK(pthread_mutex_unlock)(pthread_mutex_t *);
extern int DK(pthread_rwlock_init)(pthread_rwlock_t *, const pthread_rwlockattr_t *);
extern int DK(pthread_rwlock_destroy)(pthread_rwlock_t *);
extern int DK(pthread_rwlock_rdlock)(pthread_rwlock_t *);
extern int DK(pthread_rwlock_tryrdlock)(pthread_rwlock_t *);
extern int DK(pthread_rwlock_wrlock)(pthread_rwlock_t *);
extern int DK(pthread_rwlock_trywrlock)(pthread_rwlock_t *);
extern int DK(pthread_rwlock_unlock)(pthread_rwlock_t *);
extern int DK(pthread_cond_init)(pthread_cond_t *, const pthread_condattr_t *);
extern int DK(pthread_cond_destroy)(pthread_cond_t *);
extern int DK(pthread_cond_signal)(pthread_cond_t *);
extern int DK(pthread_cond_broadcast)(pthread_cond_t *);
extern int DK(pthread_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
extern int DK(pthread_cond_timedwait_relative_np)(pthread_cond_t *, pthread_mutex_t *,
						 const struct timespec *);
extern int DK(pthread_once)(pthread_once_t *, void (*)(void));

struct IOLock { pthread_mutex_t native; };
int linuxu_test_iolock_fail_after;
static int iomalloc_fail_after;
struct IOLock *IOLockAlloc(void)
{
	if (linuxu_test_iolock_fail_after && --linuxu_test_iolock_fail_after == 0)
		return NULL;
	struct IOLock *lock = malloc(sizeof(*lock));
	if (!lock)
		return NULL;
	assert(pthread_mutex_init(&lock->native, NULL) == 0);
	return lock;
}
void IOLockFree(struct IOLock *lock)
{
	assert(pthread_mutex_destroy(&lock->native) == 0);
	free(lock);
}
void IOLockLock(struct IOLock *lock)
{
	assert(pthread_mutex_lock(&lock->native) == 0);
}
void IOLockUnlock(struct IOLock *lock)
{
	assert(pthread_mutex_unlock(&lock->native) == 0);
}
bool IOLockTryLock(struct IOLock *lock)
{
	return pthread_mutex_trylock(&lock->native) == 0;
}
void *IOMalloc(size_t length)
{
	if (iomalloc_fail_after && --iomalloc_fail_after == 0) return NULL;
	return malloc(length);
}
void IOFree(void *address, size_t length) { (void)length; free(address); }
void IOSleep(uint64_t ms) { usleep((useconds_t)(ms * 1000)); }

/* The reentrant IODispatchQueue (dext_threads.mm): a host mutex is the
 * queue, a sleeper waits on its own record until woken or its deadline. */
struct mock_sleeper { struct mock_sleeper *next; bool woken; };
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static bool queue_missing;
static atomic_int queue_sleeps;
static atomic_int queue_wakes;
extern unsigned long dext_cond_polled;
int dext_cond_block(void **sleepers, bool (*still)(void *), void (*release)(void *), void *arg,
		    uint64_t deadline_ns)
{
	struct mock_sleeper self = { NULL, false };
	int result = 0;

	if (queue_missing)
		return ENOTSUP;
	pthread_mutex_lock(&queue_lock);
	self.next = *sleepers;
	*sleepers = &self;
	release(arg);
	while (!self.woken && still(arg)) {
		atomic_fetch_add(&queue_sleeps, 1);
		if (deadline_ns) {
			const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
			struct timespec rel;

			if (now >= deadline_ns) { result = ETIMEDOUT; break; }
			rel.tv_sec = (time_t)((deadline_ns - now) / 1000000000ULL);
			rel.tv_nsec = (long)((deadline_ns - now) % 1000000000ULL);
			pthread_cond_timedwait_relative_np(&queue_cond, &queue_lock, &rel);
		} else {
			pthread_cond_wait(&queue_cond, &queue_lock);
		}
	}
	if (!self.woken)
		for (struct mock_sleeper **link = (struct mock_sleeper **)sleepers; *link;
		     link = &(*link)->next)
			if (*link == &self) { *link = self.next; break; }
	pthread_mutex_unlock(&queue_lock);
	return result;
}
int dext_cond_wake(void **sleepers, bool all)
{
	if (queue_missing)
		return ENOTSUP;
	atomic_fetch_add(&queue_wakes, 1);
	pthread_mutex_lock(&queue_lock);
	while (*sleepers) {
		struct mock_sleeper *sleeper = *sleepers;

		*sleepers = sleeper->next;
		sleeper->woken = true;
		if (!all)
			break;
	}
	pthread_cond_broadcast(&queue_cond);
	pthread_mutex_unlock(&queue_lock);
	return 0;
}

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static atomic_int once_count;
static int counter;

static void once_body(void)
{
	atomic_fetch_add(&once_count, 1);
	usleep(10000);
}

static void *count_worker(void *unused)
{
	(void)unused;
	assert(DK(pthread_once)(&once, once_body) == 0);
	for (int i = 0; i < 10000; i++) {
		assert(DK(pthread_mutex_lock)(&counter_lock) == 0);
		counter++;
		assert(DK(pthread_mutex_unlock)(&counter_lock) == 0);
	}
	return NULL;
}

static void test_mutex_and_once(void)
{
	pthread_t workers[4];
	for (int i = 0; i < 4; i++)
		assert(pthread_create(&workers[i], NULL, count_worker, NULL) == 0);
	for (int i = 0; i < 4; i++)
		assert(pthread_join(workers[i], NULL) == 0);
	assert(counter == 40000);
	assert(atomic_load(&once_count) == 1);
	assert(DK(pthread_mutex_lock)(&counter_lock) == 0);
	assert(DK(pthread_mutex_trylock)(&counter_lock) == EBUSY);
	assert(DK(pthread_mutex_destroy)(&counter_lock) == EBUSY);
	assert(DK(pthread_mutex_unlock)(&counter_lock) == 0);
	assert(DK(pthread_mutex_destroy)(&counter_lock) == 0);
	assert(DK(pthread_mutex_lock)(&counter_lock) == EINVAL);
	pthread_mutex_t explicit_lock;
	assert(DK(pthread_mutex_init)(&explicit_lock, NULL) == 0);
	assert(DK(pthread_mutex_lock)(&explicit_lock) == 0);
	assert(DK(pthread_mutex_unlock)(&explicit_lock) == 0);
	assert(DK(pthread_mutex_destroy)(&explicit_lock) == 0);
	assert(DK(pthread_mutex_init)(&explicit_lock, (pthread_mutexattr_t *)1) == ENOTSUP);
}

static void test_rwlock(void)
{
	pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
	assert(DK(pthread_rwlock_rdlock)(&lock) == 0);
	assert(DK(pthread_rwlock_trywrlock)(&lock) == EBUSY);
	assert(DK(pthread_rwlock_tryrdlock)(&lock) == 0);
	assert(DK(pthread_rwlock_destroy)(&lock) == EBUSY);
	assert(DK(pthread_rwlock_unlock)(&lock) == 0);
	assert(DK(pthread_rwlock_unlock)(&lock) == 0);
	assert(DK(pthread_rwlock_wrlock)(&lock) == 0);
	assert(DK(pthread_rwlock_tryrdlock)(&lock) == EBUSY);
	assert(DK(pthread_rwlock_unlock)(&lock) == 0);
	assert(DK(pthread_rwlock_destroy)(&lock) == 0);
	assert(DK(pthread_rwlock_wrlock)(&lock) == EINVAL);
}

static pthread_rwlock_t shared_lock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int active_readers;
static atomic_int observed_parallel_reads;
static atomic_int writer_entered;

static void *reader_worker(void *unused)
{
	(void)unused;
	assert(DK(pthread_rwlock_rdlock)(&shared_lock) == 0);
	if (atomic_fetch_add(&active_readers, 1) >= 1)
		atomic_store(&observed_parallel_reads, 1);
	atomic_fetch_sub(&active_readers, 1);
	assert(DK(pthread_rwlock_unlock)(&shared_lock) == 0);
	return NULL;
}

static void *writer_worker(void *unused)
{
	(void)unused;
	assert(DK(pthread_rwlock_wrlock)(&shared_lock) == 0);
	atomic_store(&writer_entered, 1);
	assert(DK(pthread_rwlock_unlock)(&shared_lock) == 0);
	return NULL;
}

static void test_shared_readers(void)
{
	pthread_t reader, writer;
	assert(DK(pthread_rwlock_rdlock)(&shared_lock) == 0);
	atomic_store(&active_readers, 1);
	assert(pthread_create(&reader, NULL, reader_worker, NULL) == 0);
	assert(pthread_join(reader, NULL) == 0);
	assert(atomic_load(&observed_parallel_reads) == 1);
	assert(pthread_create(&writer, NULL, writer_worker, NULL) == 0);
	usleep(5000);
	assert(atomic_load(&writer_entered) == 0);
	atomic_store(&active_readers, 0);
	assert(DK(pthread_rwlock_unlock)(&shared_lock) == 0);
	assert(pthread_join(writer, NULL) == 0);
	assert(atomic_load(&writer_entered) == 1);
	assert(DK(pthread_rwlock_destroy)(&shared_lock) == 0);
}

static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
static atomic_int waiting;
static int ready;

static void *wait_worker(void *unused)
{
	(void)unused;
	assert(DK(pthread_mutex_lock)(&ready_lock) == 0);
	while (!ready) {
		atomic_fetch_add(&waiting, 1);
		assert(DK(pthread_cond_wait)(&ready_cond, &ready_lock) == 0);
	}
	assert(DK(pthread_mutex_unlock)(&ready_lock) == 0);
	return NULL;
}

static void test_condition(void)
{
	pthread_t worker;
	assert(pthread_create(&worker, NULL, wait_worker, NULL) == 0);
	while (!atomic_load(&waiting)) usleep(1000);
	/* Acquiring this mutex proves the waiter registered before dropping it. */
	assert(DK(pthread_mutex_lock)(&ready_lock) == 0);
	assert(DK(pthread_cond_destroy)(&ready_cond) == EBUSY);
	ready = 1;
	assert(DK(pthread_cond_signal)(&ready_cond) == 0);
	assert(DK(pthread_mutex_unlock)(&ready_lock) == 0);
	assert(pthread_join(worker, NULL) == 0);
	ready = 0;
	atomic_store(&waiting, 0);
	pthread_t broadcast_workers[2];
	for (int i = 0; i < 2; i++)
		assert(pthread_create(&broadcast_workers[i], NULL, wait_worker, NULL) == 0);
	while (atomic_load(&waiting) < 2) usleep(1000);
	assert(DK(pthread_mutex_lock)(&ready_lock) == 0);
	ready = 1;
	assert(DK(pthread_cond_broadcast)(&ready_cond) == 0);
	assert(DK(pthread_mutex_unlock)(&ready_lock) == 0);
	for (int i = 0; i < 2; i++)
		assert(pthread_join(broadcast_workers[i], NULL) == 0);
	assert(DK(pthread_mutex_lock)(&ready_lock) == 0);
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000 };
	uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
	assert(DK(pthread_cond_timedwait_relative_np)(&ready_cond, &ready_lock, &delay) == ETIMEDOUT);
	assert(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start >= 15000000);
	assert(DK(pthread_mutex_trylock)(&ready_lock) == EBUSY);
	assert(DK(pthread_mutex_unlock)(&ready_lock) == 0);
	assert(DK(pthread_cond_destroy)(&ready_cond) == 0);
	assert(DK(pthread_cond_signal)(&ready_cond) == EINVAL);
	assert(DK(pthread_mutex_destroy)(&ready_lock) == 0);
}

int main(void)
{
	/* Contended mutex operations must remain synchronized without heap
	 * backing, including trylock, destroy-busy, and subsequent destruction. */
	linuxu_test_iolock_fail_after = 1;
	assert(DK(pthread_mutex_lock)(&counter_lock) == 0);
	assert(DK(pthread_mutex_unlock)(&counter_lock) == 0);
	test_mutex_and_once();
	test_rwlock();
	test_shared_readers();
	test_condition();
	/* Waits slept on the queue: none polled. */
	assert(dext_cond_polled == 0 && atomic_load(&queue_sleeps) > 0);
	{
		/* Signals with nobody waiting do not hop onto the sleep queue. */
		pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
		const int wakes = atomic_load(&queue_wakes);

		for (int i = 0; i < 100; i++) {
			assert(DK(pthread_cond_signal)(&idle) == 0);
			assert(DK(pthread_cond_broadcast)(&idle) == 0);
		}
		assert(atomic_load(&queue_wakes) == wakes);
		assert(DK(pthread_cond_destroy)(&idle) == 0);
	}
	/* Repeat the real wait/signal/broadcast/timeout test using embedded
	 * condition state after its first IOMalloc fails. */
	ready_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	ready_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	ready = 0;
	atomic_store(&waiting, 0);
	iomalloc_fail_after = 1;
	test_condition();
	assert(dext_cond_polled == 0);
	/* Without the queue (it could not be created), waits poll, counted. */
	queue_missing = true;
	ready_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	ready_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	ready = 0;
	atomic_store(&waiting, 0);
	test_condition();
	assert(dext_cond_polled > 0);
	queue_missing = false;
	pthread_cond_t explicit_cond;
	linuxu_test_iolock_fail_after = 1;
	assert(DK(pthread_cond_init)(&explicit_cond, NULL) == 0);
	assert(DK(pthread_cond_signal)(&explicit_cond) == 0);
	assert(DK(pthread_cond_destroy)(&explicit_cond) == 0);
	pthread_mutex_t explicit_mutex;
	linuxu_test_iolock_fail_after = 1;
	assert(DK(pthread_mutex_init)(&explicit_mutex, NULL) == 0);
	assert(DK(pthread_mutex_lock)(&explicit_mutex) == 0);
	assert(DK(pthread_mutex_unlock)(&explicit_mutex) == 0);
	assert(DK(pthread_mutex_destroy)(&explicit_mutex) == 0);
	puts("dext sync mock: mutex/rwlock/cond concurrency and allocation-free fallbacks passed");
	return 0;
}
