/* linuxu: SHIM (third_party/linux/include/linux/workqueue.h)
 *
 * Workqueue API surface. Runtime: linuxu/src/work.c (per-queue worker
 * pools on linuxu threads). All non-trivial functions declared extern.
 */
#ifndef __LINUX_WORKQUEUE_H
#define __LINUX_WORKQUEUE_H

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/atomic.h>



/* ---- types ---- */
struct workqueue_struct;
struct workqueue_attrs;
struct work_struct;

/* workqueue flags used by the driver */
#define WQ_HIGHPRI	0x01
#define WQ_UNBOUND	0x02
#define WQ_FREEZABLE	0x04
#define WQ_SYSTEM	0x04
#define WQ_MEM_RECLAIM	0x08
#ifndef __WQ_ORDERED
#define __WQ_ORDERED	(1 << 17)	/* internal: workqueue is ordered */
#endif
#ifndef WQ_MAX_ACTIVE
#define WQ_MAX_ACTIVE	2048
#define WQ_DFL_ACTIVE	(WQ_MAX_ACTIVE / 2)
#endif

struct work_struct {
	atomic_long_t atomic_flags;
	struct list_head entry;
	void (*func)(struct work_struct *work);
	struct workqueue_struct *wq;
	unsigned short color;
	unsigned long queue_seq;
};

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct list_head entry;
};

/* global workqueues (kernel: kernel/workqueue.c; shim: linuxu/src/work.c).
 * Each is an independent pool, created by linuxu_workqueue_init() before
 * any module init runs. */
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_unbound_wq;
#define system_percpu_wq system_unbound_wq
extern struct workqueue_struct *system_unbound_highpri_wq;
extern struct workqueue_struct *system_freezable_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_power_wq;
extern struct workqueue_struct *system_switch_wq;
extern struct workqueue_struct *system_freezable_highpri_wq;
extern struct workqueue_struct *system_dfl_wq;
extern struct workqueue_struct *system_unbound_dfl_wq;

/* Creates the system queues and the worker manager. Idempotent and
 * thread-safe; returns 0 or a negative errno and may be retried. */
extern int linuxu_workqueue_init(void);
extern struct workqueue_struct *linuxu_default_wq(void);

typedef void (*work_func_t)(struct work_struct *work);
typedef void (*long_wq_work_func_t)(long);

#define WORK_BUSY_PENDING 1u
#define WORK_BUSY_RUNNING 2u

/* ---- init macros ---- */
#define WORK_STRUCT_FLAG_INIT(w, f)

static inline void init_work(struct work_struct *work, work_func_t func)
{
	atomic_long_set(&work->atomic_flags, 0);
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
	work->wq = NULL;
	work->color = 0;
	work->queue_seq = 0;
}

static inline void init_delayed_work(struct delayed_work *dwork,
				      work_func_t func)
{
	init_work(&dwork->work, func);
	dwork->timer = (struct timer_list) { };
	INIT_LIST_HEAD(&dwork->entry);
}

#ifndef INIT_WORK
#define INIT_WORK(_work, _func) do { init_work(_work, _func); } while (0)
#endif
#define INIT_DELAYED_WORK(_dwork, _func) \
	do { init_delayed_work(_dwork, _func); } while (0)

#define INIT_WORK(work_ptr, func) init_work(work_ptr, func)
#define INIT_WORK_ONSTACK(work_ptr, func) init_work(work_ptr, func)
#define INIT_WORK_ONSTACK_REINIT(work_ptr, func) init_work(work_ptr, func)
#define PREPARE_WORK(work_ptr, func) init_work(work_ptr, func)

#define DELAYED_WORK_INIT(name, _func) { \
	.work = (struct work_struct) { .func = _func }, \
}
#define DELAYED_WORK_INIT_ONSTACK(name, _func) DELAYED_WORK_INIT(name, _func)

#define DEFINE_WORK(name, _func) \
	struct work_struct name = { .func = _func }
#define DEFINE_WORK_ONSTACK(name, _func) \
	struct work_struct name = { .func = _func }

static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return (struct delayed_work *)work;
}

/* ---- runtime (linuxu/src/work.c) ---- */
extern bool schedule_work(struct work_struct *work);
extern bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
extern bool queue_work_node(int node, struct workqueue_struct *wq,
			    struct work_struct *work);
extern bool queue_work_on(int cpu, struct workqueue_struct *wq,
			  struct work_struct *work);
extern bool schedule_delayed_work(struct delayed_work *dwork,
				  unsigned long delay);
extern bool queue_delayed_work(struct workqueue_struct *wq,
			       struct delayed_work *work, unsigned long delay);
extern bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
				  struct delayed_work *dwork,
				  unsigned long delay);
extern bool flush_work(struct work_struct *work);
extern void flush_workqueue(struct workqueue_struct *wq);
extern bool flush_delayed_work(struct delayed_work *dwork);
extern bool cancel_work(struct work_struct *work);
extern bool cancel_work_sync(struct work_struct *work);
extern bool cancel_delayed_work_sync(struct delayed_work *dwork);
extern bool cancel_delayed_work(struct delayed_work *dwork);
extern unsigned int work_busy(struct work_struct *work);
extern int  workqueue_congested(int cpu, struct workqueue_struct *wq);

/* ---- workqueue creation (runtime: linuxu/src/work.c) ---- */
extern struct workqueue_struct *create_workqueue(const char *name);
extern struct workqueue_struct *create_singlethread_workqueue(const char *name);
extern struct workqueue_struct *alloc_workqueue(const char *name,
						unsigned int flags, int max_active);
extern struct workqueue_struct *alloc_ordered_workqueue(const char *name,
							unsigned int flags,
							int pwq_alloc);
static inline struct workqueue_struct *
linuxu_alloc_ordered_workqueue(const char *name, unsigned int flags)
{
	return alloc_ordered_workqueue(name, flags, 0);
}

/*
 * 2-arg call form (upstream CONFIG_DEBUG_OBJECTS_WORK=n); the extra
 * upstream arg is simply not passed to the shim runtime.
 */
#define alloc_ordered_workqueue(name, flags) \
	linuxu_alloc_ordered_workqueue(name, flags)

/*
 * lockdep variant (upstream alloc_ordered_workqueue_lockdep_map): the shim
 * has no lockdep, so the map pointer is unused and this just allocates.
 */
extern struct lockdep_map;
static inline struct workqueue_struct *
alloc_ordered_workqueue_lockdep_map(const char *name, unsigned int flags,
				     struct lockdep_map *map)
{
	(void)map;
	return linuxu_alloc_ordered_workqueue(name, flags);
}

extern bool mod_delayed_work(struct workqueue_struct *wq,
			     struct delayed_work *dwork,
			     unsigned long delay);
extern bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
				struct delayed_work *dwork,
				unsigned long delay);
extern void destroy_workqueue(struct workqueue_struct *wq);
extern void drain_workqueue(struct workqueue_struct *wq);
extern void workqueue_flush(int cpu);
extern bool workqueue_is_single_threaded(struct workqueue_struct *wq);

extern bool work_pending(struct work_struct *w);


static inline void destroy_work_on_stack(struct work_struct *work)
{
	(void)work;
}
#endif /* __LINUX_WORKQUEUE_H */
