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

/* wait_event and its variants, as in Linux: the waiter queues itself on
 * the wake queue, tests the condition, and sleeps (parked on its task,
 * rt/park.h) until a wake_up on the queue, a signal, or the timeout. Each
 * sleep is bounded by the backstop; a backstop that finds the condition
 * true reports the wait site once (a producer that changed the condition
 * without waking the queue). Lock variants drop and retake the caller's
 * lock around the sleep. */
#include <linux/delay.h>
#include <rt/park.h>
struct task_struct;
struct linuxu_waiter {
	struct wait_queue_head *wq;
	struct wait_queue_entry entry;
	struct task_struct *task;
	struct linuxu_wait_site *site;
	unsigned long seq;
	uint64_t deadline_ns;	/* 0: none */
	uint64_t backstop_ns;
	int state;
	bool timed, wq_locked, backstopped, expired;
};
extern void linuxu_waiter_init(struct linuxu_waiter *w, struct wait_queue_head *wq, int state,
			       long timeout, bool timed, bool wq_locked,
			       struct linuxu_wait_site *site);
extern void linuxu_waiter_prepare(struct linuxu_waiter *w);
extern void linuxu_waiter_satisfied(struct linuxu_waiter *w);
extern int linuxu_waiter_sleep(struct linuxu_waiter *w);	/* 1: the timeout passed */
extern long linuxu_waiter_left(struct linuxu_waiter *w);
extern void linuxu_waiter_finish(struct linuxu_waiter *w);

#define __linuxu_wait_event(wq_head, condition, state_, timeout_, timed_, wq_locked_, \
			    unlock_, relock_) \
({ \
    static struct linuxu_wait_site __lw_site = { __FILE__, __LINE__, 0 }; \
    struct linuxu_waiter __lw; \
    long __lw_ret; \
    linuxu_waiter_init(&__lw, &(wq_head), (state_), (long)(timeout_), (timed_), \
                       (wq_locked_), &__lw_site); \
    for (;;) { \
        int __lw_expired; \
        linuxu_waiter_prepare(&__lw); \
        if (condition) { \
            linuxu_waiter_satisfied(&__lw); \
            __lw_ret = (timed_) ? linuxu_waiter_left(&__lw) : 0; \
            break; \
        } \
        if ((state_) != TASK_UNINTERRUPTIBLE && \
            linuxu_wait_signal_pending((state_) == TASK_KILLABLE)) { \
            __lw_ret = -ERESTARTSYS; \
            break; \
        } \
        unlock_; \
        __lw_expired = linuxu_waiter_sleep(&__lw); \
        relock_; \
        if (__lw_expired) { \
            __lw_ret = (condition) ? 1 : 0; \
            break; \
        } \
    } \
    linuxu_waiter_finish(&__lw); \
    __lw_ret; \
})

#define ___wait_event(wq_head, condition, unused) \
    ((void)__linuxu_wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, false, false, , ), 0)
#define wait_event(wq_head, condition) \
    ___wait_event(wq_head, condition, 0)
#define __linuxu_wait_event_interruptible(wq_head, condition, fatal_only) \
    ((int)__linuxu_wait_event(wq_head, condition, \
        (fatal_only) ? TASK_KILLABLE : TASK_INTERRUPTIBLE, 0, false, false, , ))
#define wait_event_interruptible(wq_head, condition) \
    __linuxu_wait_event_interruptible(wq_head, condition, false)
#define wait_event_killable(wq_head, condition) \
    __linuxu_wait_event_interruptible(wq_head, condition, true)

#define wait_event_timeout(wq_head, condition, timeout) \
    __linuxu_wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, timeout, true, false, , )
#define __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, fatal_only) \
    __linuxu_wait_event(wq_head, condition, (fatal_only) ? TASK_KILLABLE : TASK_INTERRUPTIBLE, \
                        timeout, true, false, , )
#define wait_event_interruptible_timeout(wq_head, condition, timeout) \
    __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, false)
#define wait_event_killable_timeout(wq_head, condition, timeout) \
    __linuxu_wait_event_interruptible_timeout(wq_head, condition, timeout, true)
#define wait_event_interruptible_exclusive_timeout(wq_head, condition, timeout) \
    wait_event_interruptible_timeout(wq_head, condition, timeout)

#define wait_event_lock_irq(wq_head, condition, lock) \
    ((void)__linuxu_wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, false, false, \
                               spin_unlock_irq(&(lock)), spin_lock_irq(&(lock))), 0)
#define wait_event_interruptible_lock_irq(wq_head, condition, lock) \
    ((int)__linuxu_wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, false, false, \
                              spin_unlock_irq(&(lock)), spin_lock_irq(&(lock))))
/* The caller holds the wake queue's own lock. */
#define wait_event_interruptible_locked(wq_head, condition) \
    ((int)__linuxu_wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, false, true, \
                              spin_unlock(&(wq_head).lock), spin_lock(&(wq_head).lock)))
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
