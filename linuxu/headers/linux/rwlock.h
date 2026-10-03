/* linuxu: SHIM (third_party/linux/include/linux/rwlock.h) — maps to rwsem */
#ifndef __LINUX_RWLOCK_H
#define __LINUX_RWLOCK_H

#include <linux/rwsem.h>
#include <linux/spinlock.h>

typedef struct rw_semaphore rwlock_t;
typedef struct rw_semaphore read_write_lock_t;

#define __RW_LOCK_UNLOCKED(x)		RWSEM_INITIALIZER(x)
#define RW_LOCK_UNLOCKED		__RW_LOCK_UNLOCKED
#define RW_LOCK(lock)			(&(lock)->lock)
#define __init_rwsem(lock)		INIT_RWSEM(lock)
#define read_lock(rwlock)		down_read(rwlock)
#define read_unlock(rwlock)		up_read(rwlock)
#define read_trylock(rwlock)		down_read_trylock(rwlock)
#define write_lock(rwlock)		down_write(rwlock)
#define write_unlock(rwlock)		up_write(rwlock)
#define write_trylock(rwlock)		down_write_trylock(rwlock)
#define write_lock_nested(rwlock, i)	down_write(rwlock)
#define write_lock_irq(rwlock)		down_write(rwlock)
#define write_lock_irqsave(rwlock, flags)	do { (flags) = 0; down_write(rwlock); } while (0)
#define write_unlock_irq(rwlock)		up_write(rwlock)
#define write_unlock_irqrestore(rwlock, flags)	do { up_write(rwlock); local_irq_restore(flags); } while (0)
#define read_lock_nested(rwlock, i)	down_read(rwlock)
#define read_lock_irq(rwlock)		down_read(rwlock)
#define read_lock_irqsave(rwlock, flags)	do { (flags) = 0; down_read(rwlock); } while (0)
#define read_unlock_irq(rwlock)		up_read(rwlock)
#define read_unlock_irqrestore(rwlock, flags)	do { up_read(rwlock); local_irq_restore(flags); } while (0)
#define rwlock_init(rwlock)		INIT_RWSEM(rwlock)
#define rwlock_is_locked(rwlock)	rwsem_is_locked(rwlock)
#define rwlock_is_contended(rwlock)	rwsem_is_contended(rwlock)
#define rwlock_is_read_locked(rwlock)	(atomic_long_read(&(rwlock)->count) > 1)
#define rwlock_is_write_locked(rwlock)	(atomic_long_read(&(rwlock)->count) & RWSEM_WRITER_LOCKED)
#define rwlock_debug_lock(rwlock)
#define rwlock_debug_unlock(rwlock)
#define rwlock_acquire(rwlock, read, i, _cpu)
#define rwlock_release(rwlock, read)

#endif /* __LINUX_RWLOCK_H */
