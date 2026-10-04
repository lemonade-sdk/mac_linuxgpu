/* linuxu: SHIM (third_party/linux/include/linux/mutex.h)
 *
 * Mutex API surface. Maps to pthread_mutex_t in the runtime
 * (linuxu/src/sync/mutex.c). Layout follows upstream closely.
 */
#ifndef _LINUX_MUTEX_H
#define _LINUX_MUTEX_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

struct mutex_waiter {
	struct list_head list;
};

struct mutex {
	atomic_long_t   owner;
	raw_spinlock_t  wait_lock;
	struct mutex_waiter *first_waiter;
	unsigned int    linuxu_waiters;	/* parked in mutex_lock (rt/park.h) */
};

/* ---- initializers ---- */
#define __MUTEX_INITIALIZER(lockname) { .owner = LONGATOMIC_INIT(0) }
#define __MUTEX_INITIALIZER_NAMED(name, lockname) __MUTEX_INITIALIZER(lockname)

#define MUTEX_INITIALIZER_NAMED(name, lockname) \
	(struct mutex) __MUTEX_INITIALIZER_NAMED(name, lockname)

#define MUTEX_INITIALIZER(name) \
	__MUTEX_INITIALIZER_NAMED(name, name)

#define DEFINE_MUTEX(mutexname) \
	struct mutex mutexname = MUTEX_INITIALIZER(mutexname)

/* shim: owner is plain long (see atomic.h typedef); init via runtime */
extern void __mutex_init_generic(struct mutex *lock);
#define mutex_init(mutex) __mutex_init_generic(mutex)
#define mutex_init_with_key(mutex, key) __mutex_init_generic(mutex)

/* ---- runtime (linuxu/src/sync/mutex.c) ---- */
extern void mutex_destroy(struct mutex *lock);
extern bool mutex_is_locked(struct mutex *lock);
extern void mutex_lock(struct mutex *lock);
extern int  mutex_lock_interruptible(struct mutex *lock);
extern int  mutex_lock_killable(struct mutex *lock);
extern void mutex_lock_io(struct mutex *lock);
extern void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
extern int  mutex_lock_interruptible_nested(struct mutex *lock,
					    unsigned int subclass);
extern int  mutex_lock_killable_nested(struct mutex *lock,
				       unsigned int subclass);
extern void mutex_unlock(struct mutex *lock);
extern int  mutex_trylock(struct mutex *lock);

static inline bool atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *m)
{
	if (atomic_add_unless(cnt, -1, 1))
		return false;
	mutex_lock(m);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(m);
		return false;
	}
	return true;
}

#endif /* _LINUX_MUTEX_H */
