/* linuxu: SHIM (third_party/linux/include/linux/semaphore.h) */
#ifndef _LINUX_SEMAPHORE_H
#define _LINUX_SEMAPHORE_H

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/list.h>

struct semaphore {
	raw_spinlock_t		lock;
	unsigned		count;
	struct list_head	wait_list;
};

struct semaphore;

#define __SEMAPHORE_INITIALIZER(name, n) { \
		.lock = __RAW_SPIN_LOCK_UNLOCKED, \
		.count = (n), \
		.wait_list = LIST_HEAD_INIT(name.wait_list), \
	}

#define SEMAPHORE_INITIALIZER(name) __SEMAPHORE_INITIALIZER(name, 1)
#define DEFINE_SEMAPHORE(mtx, n) \
	struct semaphore mtx = __SEMAPHORE_INITIALIZER(mtx, n)

#define DECLARE_SEMAPHORE(mtx) \

extern void sema_init(struct semaphore *sem, int val);
extern void down(struct semaphore *sem);
extern int down_interruptible(struct semaphore *sem);
extern int down_killable(struct semaphore *sem);
extern int down_timeout(struct semaphore *sem, long timeout);
extern int down_trylock(struct semaphore *sem);
extern void down_read(struct rw_semaphore *sem);
extern void up(struct semaphore *sem);
extern void up_read(struct rw_semaphore *sem);
extern void up_write(struct rw_semaphore *sem);
extern void semaphore_init(struct semaphore *sem, unsigned int val);
extern void destroy_sem(struct semaphore *sem);

#endif /* _LINUX_SEMAPHORE_H */
