/* DriverKit pthread mutex/rwlock ABI adapters.
 *
 * Linux KMD and linuxu source uses the macOS pthread type layout, but a
 * DriverKit dext cannot link libSystem's pthread implementation. Store a
 * DriverKit IOLock pointer in the aligned signature field of each object.
 * Static PTHREAD_*_INITIALIZER signatures become live locks on first use.
 * Read locks share an IOLock-protected state; writers wait for readers.
 */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_TEST_DEXT_SYNC)

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Exact public DriverKit IOLib C signatures. */
struct IOLock;
extern struct IOLock *IOLockAlloc(void);
extern void IOLockFree(struct IOLock *lock);
extern void IOLockLock(struct IOLock *lock);
extern void IOLockUnlock(struct IOLock *lock);
extern bool IOLockTryLock(struct IOLock *lock);
extern void *IOMalloc(size_t length);
extern void IOFree(void *address, size_t length);
extern void IOSleep(uint64_t ms);

_Static_assert(sizeof(long) == sizeof(void *), "pthread lock slot must hold IOLock pointer");

static const pthread_mutex_t mutex_initializer = PTHREAD_MUTEX_INITIALIZER;
static const pthread_rwlock_t rwlock_initializer = PTHREAD_RWLOCK_INITIALIZER;
static const pthread_cond_t cond_initializer = PTHREAD_COND_INITIALIZER;
static const pthread_once_t once_initializer = PTHREAD_ONCE_INIT;

/* Linux void-returning mutex users cannot propagate an allocation failure.
 * A failed IOLock allocation uses the signature word as an atomic mutex.
 * These small integers cannot be valid IOLock addresses. */
#define MUTEX_FALLBACK_UNLOCKED 1L
#define MUTEX_FALLBACK_LOCKED   2L

struct dext_cond {
	struct IOLock *guard;
	uint64_t generation;
	unsigned int waiters;
	bool inline_locked;
	void *sleepers;	/* dext_cond_block's list, kept by its queue */
};

/* Blocking waits (dext/sources/dext_threads.mm): sleep on a reentrant
 * IODispatchQueue until woken. ENOTSUP when no queue exists; the wait then
 * polls (IOSleep), which dext_cond_polled counts. */
extern int dext_cond_block(void **sleepers, bool (*still)(void *),
			   void (*release)(void *), void *arg, uint64_t deadline_ns);
extern int dext_cond_wake(void **sleepers, bool all);
unsigned long dext_cond_polled;
_Static_assert(sizeof(struct dext_cond) <= sizeof(((pthread_cond_t *)0)->__opaque),
	       "pthread condition must hold fallback state");
_Static_assert(offsetof(pthread_cond_t, __opaque) % _Alignof(struct dext_cond) == 0,
	       "pthread condition fallback must be aligned");
#define COND_FALLBACK_INITIALIZING 1L

struct dext_rwlock {
	struct IOLock *guard;
	unsigned int readers;
	unsigned int waiters;
	bool writer;
};

static struct dext_rwlock *new_rwlock(void)
{
	struct dext_rwlock *state = IOMalloc(sizeof(*state));
	if (!state)
		return NULL;
	state->guard = IOLockAlloc();
	if (!state->guard) {
		IOFree(state, sizeof(*state));
		return NULL;
	}
	state->readers = 0;
	state->waiters = 0;
	state->writer = false;
	return state;
}

static void free_rwlock(struct dext_rwlock *state)
{
	IOLockFree(state->guard);
	IOFree(state, sizeof(*state));
}

static int rwlock_for(pthread_rwlock_t *r, struct dext_rwlock **out)
{
	long state;
	if (!r || !out)
		return EINVAL;
	state = __atomic_load_n(&r->__sig, __ATOMIC_ACQUIRE);
	if (state == rwlock_initializer.__sig) {
		struct dext_rwlock *created = new_rwlock();
		long expected = rwlock_initializer.__sig;
		if (!created)
			return ENOMEM;
		if (!__atomic_compare_exchange_n(&r->__sig, &expected,
					  (long)(uintptr_t)created, false,
					  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			free_rwlock(created);
		state = __atomic_load_n(&r->__sig, __ATOMIC_ACQUIRE);
	}
	if (!state || state == rwlock_initializer.__sig)
		return EINVAL;
	*out = (struct dext_rwlock *)(uintptr_t)state;
	return 0;
}

static int rwlock_acquire(pthread_rwlock_t *r, bool write, bool try_only)
{
	struct dext_rwlock *state;
	bool waiting = false;
	int ret = rwlock_for(r, &state);
	if (ret)
		return ret;
	for (;;) {
		IOLockLock(state->guard);
		/* Readers are allowed alongside other readers, including a nested
		 * read by a thread while a writer is waiting. */
		if (write ? (!state->writer && !state->readers) : !state->writer) {
			if (waiting)
				state->waiters--;
			if (write)
				state->writer = true;
			else
				state->readers++;
			IOLockUnlock(state->guard);
			return 0;
		}
		if (!try_only && !waiting) {
			state->waiters++;
			waiting = true;
		}
		IOLockUnlock(state->guard);
		if (try_only)
			return EBUSY;
		IOSleep(1);
	}
}

/* The static initializer signature is the only state in which lazy creation
 * is allowed. Explicit init installs a pointer directly. Zero is destroyed. */
static int lock_for(long *slot, long initializer, struct IOLock **out)
{
	long state;
	if (!slot || !out)
		return EINVAL;
	state = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	if (state == initializer) {
		struct IOLock *created = IOLockAlloc();
		long expected = initializer;
		long replacement = created ? (long)(uintptr_t)created :
			MUTEX_FALLBACK_UNLOCKED;
		if (!__atomic_compare_exchange_n(slot, &expected, replacement,
					  false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			if (created) IOLockFree(created);
		state = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	}
	if (state == 0 || state == initializer)
		return EINVAL;
	*out = (struct IOLock *)(uintptr_t)state;
	return 0;
}

static int init_lock(long *slot, const void *attr)
{
	struct IOLock *created;
	if (!slot)
		return EINVAL;
	if (attr)
		return ENOTSUP;
	created = IOLockAlloc();
	/* Reinitializing a live object is undefined by pthread; caller must first
	 * destroy it. A freshly declared object need not have initialized bytes. */
	__atomic_store_n(slot, created ? (long)(uintptr_t)created :
			MUTEX_FALLBACK_UNLOCKED, __ATOMIC_RELEASE);
	return 0;
}

static int destroy_lock(long *slot, long initializer)
{
	struct IOLock *lock;
	long state;
	if (!slot)
		return EINVAL;
	state = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	if (state == initializer) {
		long expected = initializer;
		return __atomic_compare_exchange_n(slot, &expected, 0, false,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : EBUSY;
	}
	if (!state)
		return EINVAL;
	if (state == MUTEX_FALLBACK_UNLOCKED || state == MUTEX_FALLBACK_LOCKED) {
		long expected = MUTEX_FALLBACK_UNLOCKED;
		return __atomic_compare_exchange_n(slot, &expected, 0, false,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : EBUSY;
	}
	lock = (struct IOLock *)(uintptr_t)state;
	if (!IOLockTryLock(lock))
		return EBUSY;
	/* Destruction concurrent with another lock operation is undefined by
	 * pthread. Mark invalid before releasing the underlying DriverKit lock. */
	__atomic_store_n(slot, 0, __ATOMIC_RELEASE);
	IOLockUnlock(lock);
	IOLockFree(lock);
	return 0;
}

static int acquire(long *slot, long initializer, bool try_only)
{
	struct IOLock *lock;
	int error = lock_for(slot, initializer, &lock);
	if (error)
		return error;
	if ((long)(uintptr_t)lock == MUTEX_FALLBACK_UNLOCKED ||
	    (long)(uintptr_t)lock == MUTEX_FALLBACK_LOCKED) {
		for (;;) {
			long expected = MUTEX_FALLBACK_UNLOCKED;
			if (__atomic_compare_exchange_n(slot, &expected,
				MUTEX_FALLBACK_LOCKED, false,
				__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
			if (expected != MUTEX_FALLBACK_LOCKED) return EINVAL;
			if (try_only) return EBUSY;
			IOSleep(1);
		}
	}
	if (try_only)
		return IOLockTryLock(lock) ? 0 : EBUSY;
	IOLockLock(lock);
	return 0;
}

static int release(long *slot, long initializer)
{
	struct IOLock *lock;
	long state;
	if (!slot)
		return EINVAL;
	state = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	if (!state || state == initializer)
		return EINVAL;
	if (state == MUTEX_FALLBACK_UNLOCKED || state == MUTEX_FALLBACK_LOCKED) {
		long expected = MUTEX_FALLBACK_LOCKED;
		return __atomic_compare_exchange_n(slot, &expected,
			MUTEX_FALLBACK_UNLOCKED, false,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED) ? 0 : EPERM;
	}
	lock = (struct IOLock *)(uintptr_t)state;
	IOLockUnlock(lock);
	return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
	return init_lock(m ? &m->__sig : NULL, attr);
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
	return destroy_lock(m ? &m->__sig : NULL, mutex_initializer.__sig);
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
	return acquire(m ? &m->__sig : NULL, mutex_initializer.__sig, false);
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
	return acquire(m ? &m->__sig : NULL, mutex_initializer.__sig, true);
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
	return release(m ? &m->__sig : NULL, mutex_initializer.__sig);
}

int pthread_rwlock_init(pthread_rwlock_t *r, const pthread_rwlockattr_t *attr)
{
	struct dext_rwlock *state;
	if (!r)
		return EINVAL;
	if (attr)
		return ENOTSUP;
	state = new_rwlock();
	if (!state)
		return ENOMEM;
	__atomic_store_n(&r->__sig, (long)(uintptr_t)state, __ATOMIC_RELEASE);
	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *r)
{
	struct dext_rwlock *state;
	long signature;
	if (!r)
		return EINVAL;
	signature = __atomic_load_n(&r->__sig, __ATOMIC_ACQUIRE);
	if (signature == rwlock_initializer.__sig) {
		long expected = signature;
		return __atomic_compare_exchange_n(&r->__sig, &expected, 0, false,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : EBUSY;
	}
	if (!signature)
		return EINVAL;
	state = (struct dext_rwlock *)(uintptr_t)signature;
	IOLockLock(state->guard);
	if (state->readers || state->writer || state->waiters) {
		IOLockUnlock(state->guard);
		return EBUSY;
	}
	/* Like pthread, concurrent use of a lock being destroyed is undefined. */
	__atomic_store_n(&r->__sig, 0, __ATOMIC_RELEASE);
	IOLockUnlock(state->guard);
	free_rwlock(state);
	return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *r)
{
	return rwlock_acquire(r, false, false);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *r)
{
	return rwlock_acquire(r, false, true);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *r)
{
	return rwlock_acquire(r, true, false);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *r)
{
	return rwlock_acquire(r, true, true);
}

int pthread_rwlock_unlock(pthread_rwlock_t *r)
{
	struct dext_rwlock *state;
	int ret = rwlock_for(r, &state);
	if (ret)
		return ret;
	IOLockLock(state->guard);
	if (state->writer)
		state->writer = false;
	else if (state->readers)
		state->readers--;
	else
		ret = EINVAL;
	IOLockUnlock(state->guard);
	return ret;
}

static struct dext_cond *new_cond(void)
{
	struct dext_cond *state = IOMalloc(sizeof(*state));
	if (!state)
		return NULL;
	state->guard = IOLockAlloc();
	if (!state->guard) {
		IOFree(state, sizeof(*state));
		return NULL;
	}
	state->generation = 0;
	state->waiters = 0;
	state->inline_locked = false;
	state->sleepers = NULL;
	return state;
}

static struct dext_cond *inline_cond(pthread_cond_t *c)
{
	struct dext_cond *state = (struct dext_cond *)(void *)c->__opaque;
	state->guard = NULL;
	state->generation = 0;
	state->waiters = 0;
	state->inline_locked = false;
	state->sleepers = NULL;
	return state;
}

static void cond_lock(struct dext_cond *state)
{
	if (state->guard) IOLockLock(state->guard);
	else while (__atomic_test_and_set(&state->inline_locked, __ATOMIC_ACQUIRE))
		IOSleep(1);
}

static void cond_unlock(struct dext_cond *state)
{
	if (state->guard) IOLockUnlock(state->guard);
	else __atomic_clear(&state->inline_locked, __ATOMIC_RELEASE);
}

static void free_cond(struct dext_cond *state)
{
	if (!state->guard) return; /* state belongs to pthread_cond_t itself */
	IOLockFree(state->guard);
	IOFree(state, sizeof(*state));
}

static int cond_for(pthread_cond_t *c, struct dext_cond **out)
{
	long state;
	if (!c || !out)
		return EINVAL;
	state = __atomic_load_n(&c->__sig, __ATOMIC_ACQUIRE);
	if (state == cond_initializer.__sig) {
		struct dext_cond *created = new_cond();
		long expected = cond_initializer.__sig;
		if (!created) {
			if (__atomic_compare_exchange_n(&c->__sig, &expected,
					COND_FALLBACK_INITIALIZING, false,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				created = inline_cond(c);
				__atomic_store_n(&c->__sig, (long)(uintptr_t)created,
						 __ATOMIC_RELEASE);
			}
		} else if (!__atomic_compare_exchange_n(&c->__sig, &expected,
					  (long)(uintptr_t)created, false,
					  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			free_cond(created);
		state = __atomic_load_n(&c->__sig, __ATOMIC_ACQUIRE);
	}
	while (state == COND_FALLBACK_INITIALIZING) {
		IOSleep(1);
		state = __atomic_load_n(&c->__sig, __ATOMIC_ACQUIRE);
	}
	if (!state || state == cond_initializer.__sig)
		return EINVAL;
	*out = (struct dext_cond *)(uintptr_t)state;
	return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
	struct dext_cond *state;
	if (!c)
		return EINVAL;
	if (attr)
		return ENOTSUP;
	state = new_cond();
	if (!state) state = inline_cond(c);
	__atomic_store_n(&c->__sig, (long)(uintptr_t)state, __ATOMIC_RELEASE);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
	struct dext_cond *state;
	long current;
	int error;
	if (!c)
		return EINVAL;
	current = __atomic_load_n(&c->__sig, __ATOMIC_ACQUIRE);
	if (current == cond_initializer.__sig) {
		long expected = current;
		return __atomic_compare_exchange_n(&c->__sig, &expected, 0, false,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : EBUSY;
	}
	error = cond_for(c, &state);
	if (error)
		return error;
	cond_lock(state);
	if (state->waiters) {
		cond_unlock(state);
		return EBUSY;
	}
	/* As with pthread, destruction concurrent with signal/wait is undefined. */
	__atomic_store_n(&c->__sig, 0, __ATOMIC_RELEASE);
	cond_unlock(state);
	free_cond(state);
	return 0;
}

static int notify_cond(pthread_cond_t *c)
{
	struct dext_cond *state;
	int error = cond_for(c, &state);
	if (error)
		return error;
	cond_lock(state);
	/* All waiters may observe this generation. Extra wakes are permitted;
	 * callers must recheck their predicate while holding their mutex. */
	state->generation++;
	cond_unlock(state);
	(void)dext_cond_wake(&state->sleepers, true);
	return 0;
}

struct cond_blocked {
	struct dext_cond *state;
	pthread_mutex_t *mutex;
	uint64_t observed;
	int unlock_error;
};

static bool cond_unsignalled(void *p)
{
	struct cond_blocked *b = p;
	bool same;

	cond_lock(b->state);
	same = b->state->generation == b->observed;
	cond_unlock(b->state);
	return same;
}

static void cond_release_mutex(void *p)
{
	struct cond_blocked *b = p;

	b->unlock_error = pthread_mutex_unlock(b->mutex);
}

int pthread_cond_signal(pthread_cond_t *c) { return notify_cond(c); }
int pthread_cond_broadcast(pthread_cond_t *c) { return notify_cond(c); }

static int wait_cond(pthread_cond_t *c, pthread_mutex_t *m,
		     const struct timespec *relative)
{
	struct dext_cond *state;
	uint64_t observed, deadline = 0;
	int error, result = 0;
	if (!m)
		return EINVAL;
	if (relative) {
		uint64_t duration, now;
		if (relative->tv_sec < 0 || relative->tv_nsec < 0 ||
		    relative->tv_nsec >= 1000000000L)
			return EINVAL;
		if ((uint64_t)relative->tv_sec >
		    (UINT64_MAX - (uint64_t)relative->tv_nsec) / 1000000000ULL)
			duration = UINT64_MAX;
		else
			duration = (uint64_t)relative->tv_sec * 1000000000ULL +
				   (uint64_t)relative->tv_nsec;
		now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
		deadline = duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
	}
	error = cond_for(c, &state);
	if (error)
		return error;
	/* Register before dropping the caller's mutex, so an intervening signal
	 * changes the generation and cannot be missed. */
	cond_lock(state);
	observed = state->generation;
	state->waiters++;
	cond_unlock(state);
	{
		/* Asleep on the queue until a signal or the deadline. */
		struct cond_blocked b = { state, m, observed, 0 };
		int blocked = dext_cond_block(&state->sleepers, cond_unsignalled,
					      cond_release_mutex, &b,
					      relative ? deadline : 0);

		if (blocked != ENOTSUP) {
			cond_lock(state);
			state->waiters--;
			cond_unlock(state);
			if (b.unlock_error)
				return b.unlock_error;
			if (blocked == EIO)
				IOSleep(1);	/* the queue refused the sleep: do not spin */
			error = pthread_mutex_lock(m);
			if (error)
				return error;
			return blocked == ETIMEDOUT ? ETIMEDOUT : 0;
		}
	}
	error = pthread_mutex_unlock(m);
	if (error) {
		cond_lock(state);
		state->waiters--;
		cond_unlock(state);
		return error;
	}
	__atomic_add_fetch(&dext_cond_polled, 1, __ATOMIC_RELAXED);
	for (;;) {
		cond_lock(state);
		if (state->generation != observed) {
			state->waiters--;
			cond_unlock(state);
			break;
		}
		if (relative && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
			state->waiters--;
			cond_unlock(state);
			result = ETIMEDOUT;
			break;
		}
		cond_unlock(state);
		IOSleep(1);
	}
	error = pthread_mutex_lock(m);
	return error ? error : result;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	return wait_cond(c, m, NULL);
}

int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m,
				       const struct timespec *relative)
{
	if (!relative)
		return EINVAL;
	return wait_cond(c, m, relative);
}

int pthread_once(pthread_once_t *once, void (*init_routine)(void))
{
	long state;
	if (!once || !init_routine)
		return EINVAL;
	for (;;) {
		state = __atomic_load_n(&once->__sig, __ATOMIC_ACQUIRE);
		if (state == 2)
			return 0;
		if (state == once_initializer.__sig) {
			long expected = state;
			if (__atomic_compare_exchange_n(&once->__sig, &expected, 1,
						false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				init_routine();
				__atomic_store_n(&once->__sig, 2, __ATOMIC_RELEASE);
				return 0;
			}
			continue;
		}
		if (state != 1)
			return EINVAL;
		IOSleep(1);
	}
}

#endif
