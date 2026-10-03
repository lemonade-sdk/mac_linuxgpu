#ifndef _LINUX_WAIT_H
#define _LINUX_WAIT_H

#include <linux/jiffies.h>

/* the wait_queue_head shape must be visible even when wait.h re-enters
 * its own body mid-cycle; the typedefs are the canonical owners */
struct spinlock;
struct list_head;

/* linuxu: SHIM (third_party/linux/include/linux/wait.h)
 *
 * Wait queue + wait_event API surface. Runtime: linuxu/src/work.c
 * (pthread condvar-based wait queues).
 */
#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/errno.h>


/* ---- wait queue entry ---- */
struct wait_queue_entry {
	unsigned long flags;
	void *private;
	int (*func)(struct wait_queue_entry *, unsigned int, int, void *);
	struct list_head entry;
};

/* vendor 2026 wait.h: the waitqueue entry typedef + active() probe */
typedef struct wait_queue_entry wait_queue_entry_t;

struct wait_queue_head {
	spinlock_t lock;
	struct list_head head;
};

typedef struct wait_queue_head wait_queue_head_t;

static inline bool waitqueue_active(struct wait_queue_head *wq_head)
{
	return !list_empty(&wq_head->head);
}
typedef int (*wake_function_t)(struct wait_queue_entry *wq_entry,
			       unsigned mode, int flags, void *key);

/* ---- init / create (runtime: linuxu/src/work.c) ---- */
#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { \
	.lock = ___SPIN_LOCK_INITIALIZER(name.lock), .head = LIST_HEAD_INIT(name.head) }

#define DECLARE_WAIT_QUEUE_HEAD(name) \
	struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

#define DECLARE_WAIT_QUEUE_HEAD_NAME(name, label) \
	struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

extern void init_waitqueue_head(struct wait_queue_head *wq_head);
extern struct wait_queue_head *alloc_wait_queue_head(gfp_t flags);
extern struct wait_queue_head *alloc_wait_queue_head_node(gfp_t flags, int node);
extern void destroy_wait_queue_head(struct wait_queue_head *wq_head);

/* ---- waitqueue operations (runtime: linuxu/src/work.c) ---- */
extern void __add_wait_queue(struct wait_queue_head *wq_head,
			     struct wait_queue_entry *wq_entry);
extern void __add_wait_queue_exclusive(struct wait_queue_head *wq_head,
				       struct wait_queue_entry *wq_entry);
extern void __remove_wait_queue(struct wait_queue_head *wq_head,
				struct wait_queue_entry *wq_entry);
extern int  wait_queue_active(struct wait_queue_head *wq_head);

static inline void add_wait_queue(struct wait_queue_head *wq_head,
				  struct wait_queue_entry *wq_entry)
{
	spin_lock(&wq_head->lock);
	__add_wait_queue(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}
static inline void add_wait_queue_exclusive(struct wait_queue_head *wq_head,
					    struct wait_queue_entry *wq_entry)
{
	spin_lock(&wq_head->lock);
	__add_wait_queue_exclusive(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}
static inline void remove_wait_queue(struct wait_queue_head *wq_head,
				     struct wait_queue_entry *wq_entry)
{
	spin_lock(&wq_head->lock);
	__remove_wait_queue(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}

/* ---- prepare / finish ---- */
extern void prepare_to_wait(struct wait_queue_head *wq_head,
			    struct wait_queue_entry *wq_entry, int state);
extern long prepare_to_wait_event(struct wait_queue_head *wq_head,
				  struct wait_queue_entry *wq_entry, int state);
extern void finish_wait(struct wait_queue_head *wq_head,
			struct wait_queue_entry *wq_entry);

/* ---- wake up (runtime: linuxu/src/work.c) ---- */
#define TASK_RUNNING	0
#define TASK_PARKED	0x40
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_KILLABLE		4

extern unsigned int wake_up_interruptible(struct wait_queue_head *wq_head);
extern unsigned int wake_up_interruptible_sync(struct wait_queue_head *wq_head);
extern unsigned int wake_up_interruptible_poll(struct wait_queue_head *wq_head,
					       unsigned int mode);
extern unsigned int wake_up(struct wait_queue_head *wq_head);
extern unsigned int wake_up_all(struct wait_queue_head *wq_head);
extern unsigned int wake_up_interruptible_nr(struct wait_queue_head *wq_head,
					     unsigned int nr);
extern unsigned int wake_up_all_nr(struct wait_queue_head *wq_head,
				   unsigned int nr);
#define WQ_FLAG_EXCLUSIVE 0x01
extern int default_wake_function(struct wait_queue_entry *, unsigned int, int, void *);
extern int autoremove_wake_function(struct wait_queue_entry *, unsigned int, int, void *);
extern bool linuxu_wait_signal_pending(bool fatal_only);
static inline void init_waitqueue_entry(struct wait_queue_entry *entry, struct task_struct *task)
{
	entry->flags = 0; entry->private = task; entry->func = default_wake_function;
	INIT_LIST_HEAD(&entry->entry);
}
static inline void init_waitqueue_func_entry(struct wait_queue_entry *entry, wake_function_t function)
{
	entry->flags = 0; entry->private = NULL; entry->func = function;
	INIT_LIST_HEAD(&entry->entry);
}

/* ---- schedule / sleep ---- */
extern long schedule_timeout(long timeout);
extern long schedule_timeout_interruptible(long timeout);
extern long schedule_timeout_killable(long timeout);
extern long schedule_timeout_uninterruptible(long timeout);
extern void  schedule(void);
extern int   schedule_tail(void);

/* DriverKit has no Linux task signals. Poll conditions between bounded
 * sleeps; a timeout is measured once, and lock variants return with the
 * caller's lock held. Wakeup producers publish state before notifying. */
#include <linux/delay.h>
#define ___wait_event(wq_head, condition, unused) \
({ \
    (void)(&(wq_head)); \
    while (!(condition)) \
        msleep(1); \
    0; \
})
#define wait_event(wq_head, condition) \
    ___wait_event(wq_head, condition, 0)
#define __linuxu_wait_event_interruptible(wq_head, condition, fatal_only) \
({ \
    int __result = 0; (void)(&(wq_head)); \
    while (!(condition)) { \
        if (linuxu_wait_signal_pending(fatal_only)) { __result = -ERESTARTSYS; break; } \
        msleep(1); \
    } \
    __result; \
})
#define wait_event_interruptible(wq_head, condition) \
    __linuxu_wait_event_interruptible(wq_head, condition, false)
#define wait_event_killable(wq_head, condition) \
    __linuxu_wait_event_interruptible(wq_head, condition, true)

#define wait_event_timeout(wq_head, condition, timeout) \
({ \
    long __left = (timeout); \
    unsigned long __end = jiffies + (__left > 0 ? __left : 0); \
    (void)(&(wq_head)); \
    for (;;) { \
        unsigned long __now = jiffies; \
        __left = time_before(__now, __end) ? (long)(__end - __now) : 0; \
        if (condition) { \
            if (__left <= 0) __left = 1; \
            break; \
        } \
        if (__left <= 0) { \
            __left = 0; \
            break; \
        } \
        msleep(1); \
    } \
    __left; \
})
#define __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, fatal_only) \
({ \
    long __result = wait_event_timeout(wq_head, \
        (condition) || linuxu_wait_signal_pending(fatal_only), timeout); \
    if (!(condition) && linuxu_wait_signal_pending(fatal_only)) __result = -ERESTARTSYS; \
    __result; \
})
#define wait_event_interruptible_timeout(wq_head, condition, timeout) \
    __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, false)
#define wait_event_killable_timeout(wq_head, condition, timeout) \
    __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, true)
#define wait_event_interruptible_exclusive_timeout(wq_head, condition, timeout) \
    wait_event_interruptible_timeout(wq_head, condition, timeout)

#define wait_event_lock_irq(wq_head, condition, lock) \
({ \
    (void)(&(wq_head)); \
    while (!(condition)) { \
        spin_unlock_irq(&(lock)); \
        msleep(1); \
        spin_lock_irq(&(lock)); \
    } \
    0; \
})
#define wait_event_interruptible_lock_irq(wq_head, condition, lock) \
({ \
    int __result = 0; (void)(&(wq_head)); \
    while (!(condition)) { \
        if (linuxu_wait_signal_pending(false)) { __result = -ERESTARTSYS; break; } \
        spin_unlock_irq(&(lock)); msleep(1); spin_lock_irq(&(lock)); \
    } \
    __result; \
})
#define wait_event_interruptible_locked(wq_head, condition) \
    wait_event_interruptible_lock_irq(wq_head, condition, (wq_head).lock)
extern unsigned int wake_up_all_locked(struct wait_queue_head *wq_head);
#define wake_up_interruptible_sync_poll(wq_head, mode, nr) \
	wake_up_interruptible_poll(wq_head, mode)


#endif /* _LINUX_WAIT_H */

#ifndef _LINUX_WAIT_H_EXTRA
#define _LINUX_WAIT_H_EXTRA
/* wait-on-bit surface (kfd_events.c) — init_wait per vendor 2026 wait.h */
struct task_struct;
#define init_wait(wait) \
	do { \
		(wait)->private = current; \
		(wait)->func = autoremove_wake_function; \
		INIT_LIST_HEAD(&(wait)->entry); \
		(wait)->flags = 0; \
	} while (0)
static inline int out_of_line_wait_on_bit(void *word, unsigned long bit,
					  int (*fn)(void *), unsigned mode)
{
	(void)word; (void)bit; (void)fn; (void)mode;
	return 0;
}
static inline bool fatal_signal_pending(struct task_struct *t)
{
	extern bool linuxu_task_fatal_signal_pending(struct task_struct *);
	return linuxu_task_fatal_signal_pending(t);
}
#endif
