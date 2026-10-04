/* Allocation-free read/write semaphores. The Linux API cannot report an
 * allocation failure from init/down, so lock state lives in the semaphore. */
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <rt/park.h>

/* Contended: park on the semaphore until its count or its waiting writers
 * change (every release unparks it while anyone waits), bounded by the
 * backstop (rt/park.h). */
struct rwsem_snapshot {
	struct rw_semaphore *sem;
	long count, writers;
};

static bool rwsem_unchanged(const void *p)
{
	const struct rwsem_snapshot *s = p;

	return atomic_long_read(&s->sem->count) == s->count &&
	       atomic_long_read(&s->sem->writers_waiting) == s->writers;
}

struct rwsem_sleep {
	uint64_t backstop;
	bool backstopped;
};

static void rwsem_sleep(struct rw_semaphore *sem, struct rwsem_sleep *z)
{
	struct rwsem_snapshot s = { sem, atomic_long_read(&sem->count),
				    atomic_long_read(&sem->writers_waiting) };

	if (!z->backstop)
		z->backstop = linuxu_park_backstop_first();
	if (linuxu_park(sem, rwsem_unchanged, &s, linuxu_park_now_ns() + z->backstop) ==
	    LINUXU_PARK_WOKEN) {
		z->backstopped = false;
		z->backstop = linuxu_park_backstop_first();
	} else {
		z->backstopped = true;
		z->backstop = linuxu_park_backstop_next(z->backstop);
	}
}

static void rwsem_acquired(const struct rwsem_sleep *z, const void *caller)
{
	if (z->backstopped)
		linuxu_wait_missed(NULL, caller);
}

/* After a release, or a waiting writer leaving: wake the sleepers. */
static void rwsem_wake(struct rw_semaphore *sem)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (atomic_long_read(&sem->waiters))
		linuxu_unpark(sem);
}

static int rwsem_try_read(struct rw_semaphore *sem)
{
	long count;
	if (atomic_long_read(&sem->writers_waiting))
		return 0;
	count = atomic_long_read(&sem->count);
	while (!(count & RWSEM_WRITER_LOCKED)) {
		long previous = atomic_long_cmpxchg(&sem->count, count, count + 2);
		if (previous == count)
			return 1;
		count = previous;
	}
	return 0;
}

void rwsem_init(struct rw_semaphore *sem)
{
	atomic_long_set(&sem->count, RWSEM_UNLOCKED_VALUE);
	atomic_long_set(&sem->owner, 0);
	atomic_long_set(&sem->waiters, 0);
	atomic_long_set(&sem->writers_waiting, 0);
}

void down_read(struct rw_semaphore *sem)
{
	struct rwsem_sleep z = { 0 };

	if (rwsem_try_read(sem))
		return;
	atomic_long_inc(&sem->waiters);
	while (!rwsem_try_read(sem))
		rwsem_sleep(sem, &z);
	atomic_long_dec(&sem->waiters);
	rwsem_acquired(&z, __builtin_return_address(0));
}

void down_write(struct rw_semaphore *sem)
{
	struct rwsem_sleep z = { 0 };

	atomic_long_inc(&sem->waiters);
	atomic_long_inc(&sem->writers_waiting);
	while (atomic_long_cmpxchg(&sem->count, RWSEM_UNLOCKED_VALUE,
				 RWSEM_WRITER_LOCKED) != RWSEM_UNLOCKED_VALUE)
		rwsem_sleep(sem, &z);
	atomic_long_dec(&sem->writers_waiting);
	atomic_long_dec(&sem->waiters);
	/* Readers held off by this writer's wait may go once it holds the
	 * lock and releases it; nothing to wake until then. */
	rwsem_acquired(&z, __builtin_return_address(0));
}

int down_read_trylock(struct rw_semaphore *sem)
{
	return rwsem_try_read(sem);
}

int down_write_trylock(struct rw_semaphore *sem)
{
	return atomic_long_cmpxchg(&sem->count, RWSEM_UNLOCKED_VALUE,
				  RWSEM_WRITER_LOCKED) == RWSEM_UNLOCKED_VALUE;
}

static int down_interruptible_rw(struct rw_semaphore *sem, bool write, bool fatal_only)
{
	struct rwsem_sleep z = { 0 };

	atomic_long_inc(&sem->waiters);
	if (write) atomic_long_inc(&sem->writers_waiting);
	while (!(write ? down_write_trylock(sem) : rwsem_try_read(sem))) {
		if (linuxu_wait_signal_pending(fatal_only)) {
			if (write) atomic_long_dec(&sem->writers_waiting);
			atomic_long_dec(&sem->waiters);
			if (write) rwsem_wake(sem);	/* readers it held off */
			return -EINTR;
		}
		rwsem_sleep(sem, &z);
	}
	if (write) atomic_long_dec(&sem->writers_waiting);
	atomic_long_dec(&sem->waiters);
	rwsem_acquired(&z, __builtin_return_address(0));
	return 0;
}
int down_read_killable(struct rw_semaphore *sem) { return down_interruptible_rw(sem, false, true); }
int down_read_interruptible(struct rw_semaphore *sem) { return down_interruptible_rw(sem, false, false); }
int down_write_killable(struct rw_semaphore *sem) { return down_interruptible_rw(sem, true, true); }

void up_read(struct rw_semaphore *sem)
{
	atomic_long_sub(2, &sem->count);
	rwsem_wake(sem);
}

void up_write(struct rw_semaphore *sem)
{
	atomic_long_set(&sem->count, RWSEM_UNLOCKED_VALUE);
	rwsem_wake(sem);
}

void downgrade_write(struct rw_semaphore *sem)
{
	/* The writer becomes one reader (readers count in steps of two);
	 * waiting writers stay excluded until it calls up_read. */
	atomic_long_set(&sem->count, 2);
	rwsem_wake(sem);
}

void rwsem_destroy(struct rw_semaphore *sem)
{
	if (!atomic_long_read(&sem->count) && !atomic_long_read(&sem->waiters))
		rwsem_init(sem);
}

bool rwsem_is_contended(struct rw_semaphore *sem)
{
	return atomic_long_read(&sem->waiters) > 0;
}

/* ---- classic semaphore (struct semaphore: count + wait list) ---- */
/*
 * The shim's struct semaphore is a plain counting semaphore
 * (raw_spinlock + count + wait list).  We keep a side table of
 * pthread mutexes for blocking, and count the semaphore state in
 * sem->count (unsigned).
 */
#include <pthread.h>
#include <linux/semaphore.h>
#include <linux/delay.h>

static pthread_mutex_t *sema_mu(void)
{
	static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	return &m;
}

void sema_init(struct semaphore *sem, int val)
{
	pthread_mutex_lock(sema_mu());
	sem->count = (unsigned)val;
	pthread_mutex_unlock(sema_mu());
}

static bool sema_empty(const void *p)
{
	return !__atomic_load_n(&((const struct semaphore *)p)->count, __ATOMIC_SEQ_CST);
}

/* Park until up() (which unparks), bounded by the backstop. */
static int sema_wait(struct semaphore *sem, int interruptible, const void *caller)
{
	uint64_t backstop = linuxu_park_backstop_first();
	bool backstopped = false;

	while (down_trylock(sem)) {
		if (interruptible && linuxu_wait_signal_pending(interruptible == 2))
			return -EINTR;
		if (linuxu_park(sem, sema_empty, sem, linuxu_park_now_ns() + backstop) ==
		    LINUXU_PARK_WOKEN) {
			backstopped = false;
			backstop = linuxu_park_backstop_first();
		} else {
			backstopped = true;
			backstop = linuxu_park_backstop_next(backstop);
		}
	}
	if (backstopped)
		linuxu_wait_missed(NULL, caller);
	return 0;
}

void down(struct semaphore *sem)
{
	(void)sema_wait(sem, 0, __builtin_return_address(0));
}

static int sem_interruptible(struct semaphore *sem, bool fatal_only)
{
	return sema_wait(sem, fatal_only ? 2 : 1, __builtin_return_address(0));
}
int down_interruptible(struct semaphore *sem) { return sem_interruptible(sem, false); }
int down_killable(struct semaphore *sem) { return sem_interruptible(sem, true); }


void up(struct semaphore *sem)
{
	pthread_mutex_lock(sema_mu());
	sem->count++;
	pthread_mutex_unlock(sema_mu());
	linuxu_unpark(sem);
}

int down_trylock(struct semaphore *sem)
{
	pthread_mutex_lock(sema_mu());
	if (sem->count == 0) {
		pthread_mutex_unlock(sema_mu());
		return 1;
	}
	sem->count--;
	pthread_mutex_unlock(sema_mu());
	return 0;
}
