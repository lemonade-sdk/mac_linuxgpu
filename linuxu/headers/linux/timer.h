/* linuxu: SHIM (third_party/linux/include/linux/timer.h)
 *
 * Timer API surface. Runtime: linuxu/src/sched/timer.c (or merged
 * into workqueue.c). The timer_list layout follows upstream
 * (timer_types.h inlined here).
 */
#ifndef _LINUX_TIMER_H
#define _LINUX_TIMER_H

#include <linux/types.h>
#include <linux/list.h>



/*
 * struct timer_list - from upstream timer_types.h.
 */
struct timer_list {
	struct hlist_node entry;
	unsigned long     expires;
	void            (*function)(struct timer_list *);
	u32             flags;
	unsigned int    deleting;
	bool shutdown;
};

/* ---- flags ---- */
#define TIMER_DEFERRABLE	0x00080000
#define TIMER_PINNED		0x00100000
#define TIMER_IRQSAFE		0x00200000
#define TIMER_INIT_FLAGS	(TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)

/* ---- initializers ---- */
#define __TIMER_INITIALIZER(_function, _flags) { \
		.entry = { NULL, NULL }, \
		.function = (_function), \
		.flags = (_flags), \
	}

#define DEFINE_TIMER(_name, _function) \
	struct timer_list _name = __TIMER_INITIALIZER(_function, 0)

static inline void timer_setup(struct timer_list *timer,
			       void (*func)(struct timer_list *),
			       unsigned int flags)
{
	timer->function = func;
	timer->flags = flags;
	timer->entry.next = NULL;
	timer->entry.pprev = NULL;
	timer->expires = 0;
	timer->deleting = 0;
	timer->shutdown = false;
}

static inline void setup_timer(struct timer_list *timer,
			       void (*func)(struct timer_list *),
			       unsigned int flags)
{
	timer_setup(timer, func, flags);
}

static inline void timer_init(struct timer_list *timer,
			      void (*func)(struct timer_list *),
			      unsigned int flags)
{
	timer_setup(timer, func, flags);
}

static inline void timer_init_on_stack(struct timer_list *timer,
				       void (*func)(struct timer_list *),
				       unsigned int flags)
{
	timer_setup(timer, func, flags);
}

extern int timer_pending(const struct timer_list *timer);
/* Bootstrap must establish the service before accepting hardware work. */
extern int linuxu_timer_service_init(void);
#define timer_setup_on_stack(timer, function, flags) timer_setup(timer, function, flags)
#define timer_destroy_on_stack(timer) timer_destroy(timer)

/* ---- runtime (linuxu/src/sched/timer.c) ---- */
extern void add_timer(struct timer_list *timer);
extern void add_timer_on(struct timer_list *timer, int cpu);
extern int  mod_timer(struct timer_list *timer, unsigned long expires);
extern int  mod_timer_pending(struct timer_list *timer, unsigned long expires);
extern int  mod_timer_on(struct timer_list *timer, unsigned long expires, int cpu);
extern int  del_timer(struct timer_list *timer);
extern int  del_timer_sync(struct timer_list *timer);
/* vendor 2026: timer_delete() returns bool (timer was pending) */
extern bool timer_delete(struct timer_list *timer);
extern bool timer_delete_sync(struct timer_list *timer);
extern int timer_delete_sync_try(struct timer_list *timer);
extern int timer_shutdown(struct timer_list *timer);
extern int timer_shutdown_sync(struct timer_list *timer);
extern void timer_start(struct timer_list *timer, unsigned long expires);
extern void timer_start_on(struct timer_list *timer, unsigned long expires, int cpu);
extern int  timer_reduce(struct timer_list *timer, unsigned long expires);
extern void timer_destroy(struct timer_list *timer);

#endif /* _LINUX_TIMER_H */
#ifndef timer_container_of
#define timer_container_of(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#endif
