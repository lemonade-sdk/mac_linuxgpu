/* linuxu: SHIM (third_party/linux/include/linux/completion.h) */
#ifndef __LINUX_COMPLETION_H
#define __LINUX_COMPLETION_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/debug_locks.h>
/* wait.h is NOT included: it is part of the kernel.h -> sched.h ->
 * mm.h -> ... -> completion.h cycle. The names it provides that this
 * header needs are re-declared here in the exact same shape. */
struct wait_queue_head;
struct wait_queue_entry;
struct task_struct;
struct wait_bit_action;
typedef struct wait_queue_head wait_queue_head_t;
typedef int (*wake_function_t)(struct wait_queue_entry *wq_entry,
			       unsigned mode, int sync, void *arg);

/* forward decls for the wait queue hooks (wait.h cannot include
 * completion.h back — circular) */
struct wait_queue_entry;
struct task_struct;


struct completion {
	unsigned int	done;
	raw_spinlock_t	lock;
	struct list_head	wait;
};


#define COMPLETION_INITIALIZER(name) { \
	.done = 0, \
	.lock = __RAW_SPIN_LOCK_UNLOCKED, \
	.wait = LIST_HEAD_INIT(name.wait), \
}

#define DECLARE_COMPLETION(name) \
	struct completion name = COMPLETION_INITIALIZER(name)

#define DECLARE_COMPLETION_ONSTACK(name) \
	struct completion name = COMPLETION_INITIALIZER(name)

#define DECLARE_COMPLETION_INITIALIZER(name, val) \
	struct completion name = COMPLETION_INITIALIZER(name)

#define INIT_COMPLETION(x) do { \
	(x).done = 0; \
	raw_spinlock_init(&(x).lock); \
	INIT_LIST_HEAD(&(x).wait); \
} while (0)

extern void init_completion(struct completion *x);
extern void init_completion_done(struct completion *x);

/* Reinitialization resets the event, preserving its existing wait state. */
extern void reinit_completion(struct completion *x);
extern void complete(struct completion *x);
extern void complete_all(struct completion *x);
extern void complete_done(struct completion *x);
extern int completion_done(struct completion *x);
extern int wait_for_completion(struct completion *x);
extern unsigned long wait_for_completion_timeout(struct completion *x,
						 unsigned long timeout);
extern int wait_for_completion_killable(struct completion *x);
extern long wait_for_completion_killable_timeout(struct completion *x,
							  unsigned long timeout);
extern int wait_for_completion_interruptible(struct completion *x);
extern long wait_for_completion_interruptible_timeout(struct completion *x,
								unsigned long timeout);
extern bool wait_for_completion_interruptible_wake_function(struct completion *x,
							     wake_function_t wake_function);
extern bool wait_for_completion_wake_function(struct completion *x,
					       wake_function_t wake_function);
extern void rearm_completion(struct completion *x);
extern bool try_wait_for_completion(struct completion *x);
extern void complete_and_exit(struct completion *x, unsigned long v);

#endif /* __LINUX_COMPLETION_H */
