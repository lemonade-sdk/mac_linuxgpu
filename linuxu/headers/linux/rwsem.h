/* linuxu: SHIM (third_party/linux/include/linux/rwsem.h)
 *
 * Read/write semaphore API surface. Runtime: linuxu/src/rwsem.c
 * (allocation-free reader/writer state).
 */
#ifndef __LINUX_RWSEM_H
#define __LINUX_RWSEM_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

struct rwsem_waiter {
	struct list_head list;
};

struct rw_semaphore {
	atomic_long_t   count;
	atomic_long_t   owner;
	atomic_long_t   waiters;
	atomic_long_t   writers_waiting;
	raw_spinlock_t  wait_lock;
	struct rwsem_waiter *first_waiter;
};

#define RWSEM_UNLOCKED_VALUE		0UL
#define RWSEM_WRITER_LOCKED		(1UL << 0)

#define RWSEM_INITIALIZER(name) \
	(struct rw_semaphore) { \
		.count   = LONGATOMIC_INIT(RWSEM_UNLOCKED_VALUE), \
		.owner   = LONGATOMIC_INIT(0), \
	}

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = RWSEM_INITIALIZER(name)

#define INIT_RWSEM(name) rwsem_init(name)

static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return atomic_long_read(&sem->count) != RWSEM_UNLOCKED_VALUE;
}

/* ---- runtime (linuxu/src/rwsem.c) ---- */
extern void down_read(struct rw_semaphore *sem);
extern void down_write(struct rw_semaphore *sem);
extern int  down_read_trylock(struct rw_semaphore *sem);
extern int  down_write_trylock(struct rw_semaphore *sem);
extern int  down_read_interruptible(struct rw_semaphore *sem);

/* non-sleeping down()/up() pair (upstream down_interruptible is a
 * blocking down(); the userspace shim treats both the same) */
extern int  down_read_killable(struct rw_semaphore *sem);
extern int  down_write_killable(struct rw_semaphore *sem);
extern void up_read(struct rw_semaphore *sem);
extern void up_write(struct rw_semaphore *sem);
/* Atomically convert a held write lock into a read lock. */
extern void downgrade_write(struct rw_semaphore *sem);
extern void rwsem_destroy(struct rw_semaphore *sem);
extern bool rwsem_is_contended(struct rw_semaphore *sem);
extern void rwsem_init(struct rw_semaphore *sem);

#define init_rwsem(sem) rwsem_init(sem)

#endif /* __LINUX_RWSEM_H */
