/* linuxu: SHIM (third_party/linux/include/linux/rtmutex.h) - rwsem-backed for userspace */
#ifndef _LINUX_RT_MUTEX_H
#define _LINUX_RT_MUTEX_H

#include <linux/rwsem.h>
#include <linux/types.h>

struct rt_mutex {
	struct rw_semaphore lock;
};

struct rt_mutex_base {
	struct rw_semaphore lock;
};

#define __RT_MUTEX_INITIALIZER(lockname) { .lock = RWSEM_INITIALIZER(lockname.lock) }
#define DEFINE_RT_MUTEX(name) struct rt_mutex name = __RT_MUTEX_INITIALIZER(name)
#define RT_MUTEX_INITIALIZER(name)	__RT_MUTEX_INITIALIZER(name)

static inline void rt_mutex_init(struct rt_mutex *m)
{
	init_rwsem(&m->lock);
}

/* Exclusive sleeping lock; no priority inheritance in userspace. */
static inline void rt_mutex_lock(struct rt_mutex *lock)
{
	down_write(&lock->lock);
}
static inline void rt_mutex_unlock(struct rt_mutex *lock)
{
	up_write(&lock->lock);
}
static inline int rt_mutex_trylock(struct rt_mutex *lock)
{
	return down_write_trylock(&lock->lock);
}
static inline int rt_mutex_lock_interruptible(struct rt_mutex *lock)
{
	down_write(&lock->lock);
	return 0;
}

static inline void rt_mutex_lock_nested(struct rt_mutex *lock, int n)
{
	rt_mutex_lock(lock);
}
static inline int rt_mutex_trylock_nested(struct rt_mutex *lock, int n)
{
	return rt_mutex_trylock(lock);
}

#endif /* _LINUX_RT_MUTEX_H */
