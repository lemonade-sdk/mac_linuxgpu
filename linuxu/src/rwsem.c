/* Allocation-free read/write semaphores. The Linux API cannot report an
 * allocation failure from init/down, so lock state lives in the semaphore. */
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/wait.h>

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
	atomic_long_inc(&sem->waiters);
	while (!rwsem_try_read(sem))
		usleep_range(50, 100);
	atomic_long_dec(&sem->waiters);
}

void down_write(struct rw_semaphore *sem)
{
	atomic_long_inc(&sem->waiters);
	atomic_long_inc(&sem->writers_waiting);
	while (atomic_long_cmpxchg(&sem->count, RWSEM_UNLOCKED_VALUE,
				 RWSEM_WRITER_LOCKED) != RWSEM_UNLOCKED_VALUE)
		usleep_range(50, 100);
	atomic_long_dec(&sem->writers_waiting);
	atomic_long_dec(&sem->waiters);
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
	atomic_long_inc(&sem->waiters);
	if (write) atomic_long_inc(&sem->writers_waiting);
	while (!(write ? down_write_trylock(sem) : rwsem_try_read(sem))) {
		if (linuxu_wait_signal_pending(fatal_only)) {
			if (write) atomic_long_dec(&sem->writers_waiting);
			atomic_long_dec(&sem->waiters);
			return -EINTR;
		}
		usleep_range(50, 100);
	}
	if (write) atomic_long_dec(&sem->writers_waiting);
	atomic_long_dec(&sem->waiters);
	return 0;
}
int down_read_killable(struct rw_semaphore *sem) { return down_interruptible_rw(sem, false, true); }
int down_read_interruptible(struct rw_semaphore *sem) { return down_interruptible_rw(sem, false, false); }
int down_write_killable(struct rw_semaphore *sem) { return down_interruptible_rw(sem, true, true); }

void up_read(struct rw_semaphore *sem)
{
	atomic_long_sub(2, &sem->count);
}

void up_write(struct rw_semaphore *sem)
{
	atomic_long_set(&sem->count, RWSEM_UNLOCKED_VALUE);
}

void downgrade_write(struct rw_semaphore *sem)
{
	/* The writer becomes one reader (readers count in steps of two);
	 * waiting writers stay excluded until it calls up_read. */
	atomic_long_set(&sem->count, 2);
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

void down(struct semaphore *sem)
{
	pthread_mutex_lock(sema_mu());
	while (sem->count == 0) {
		/* naive busy wait with a yield; the KMD uses semaphores
		 * only for the legacy task_barrier path (P2) */
		pthread_mutex_unlock(sema_mu());
		usleep_range(100, 200);
		pthread_mutex_lock(sema_mu());
	}
	sem->count--;
	pthread_mutex_unlock(sema_mu());
}

static int sem_interruptible(struct semaphore *sem, bool fatal_only)
{
	while (down_trylock(sem)) {
		if (linuxu_wait_signal_pending(fatal_only)) return -EINTR;
		usleep_range(100, 200);
	}
	return 0;
}
int down_interruptible(struct semaphore *sem) { return sem_interruptible(sem, false); }
int down_killable(struct semaphore *sem) { return sem_interruptible(sem, true); }


void up(struct semaphore *sem)
{
	pthread_mutex_lock(sema_mu());
	sem->count++;
	pthread_mutex_unlock(sema_mu());
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
