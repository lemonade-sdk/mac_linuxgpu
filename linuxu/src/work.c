/* Linux workqueues over linuxu threads.
 *
 * Every queue owns a small worker pool.  Ordered queues (max_active == 1,
 * __WQ_ORDERED, alloc_ordered_workqueue, create_singlethread_workqueue) run
 * at most one item at a time in strict FIFO order.  Other queues run up to
 * max_active items concurrently (0 selects WQ_DFL_ACTIVE), capped at
 * LINUXU_WQ_MAX_WORKERS.  A work item never runs concurrently with itself,
 * on any queue.
 *
 * Workers are created on demand by one manager thread and retire after
 * LINUXU_WQ_IDLE_MS without work, so idle queues hold no thread.  This
 * matters in the dext, where every pthread is a DriverKit dispatch queue
 * drawn from a fixed slot table (shims/dext_threads.c).  The manager also
 * moves expired delayed work onto its queue, independent of how busy the
 * queue's workers are, and joins retired workers.  Producers never create
 * threads, so queue_work() stays cheap from interrupt-style callers.
 *
 * All queue, work and barrier state is protected by work_lock so a work item
 * can move between queues.  A callback may free its work container, so
 * running state and flush barriers live outside the container and are never
 * dereferenced through the work pointer after the callback returns.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/workqueue.h>
#undef alloc_ordered_workqueue
#include <linux/jiffies.h>
#include <linux/kern_levels.h>

extern int printk(const char *fmt, ...);

#define WORK_PENDING  1L
#define WORK_DELAYED  4L

/* Concurrency of one non-ordered queue.  Linux allows WQ_DFL_ACTIVE; a
 * userspace dext has a fixed thread budget, so the pool stops at 16. */
#define LINUXU_WQ_MAX_WORKERS		16
/* Workers beyond the first of each queue, summed over all queues.  The
 * first worker of a queue is never refused, so every queue makes progress. */
#define LINUXU_WQ_SHARED_WORKERS	48
#define LINUXU_WQ_IDLE_MS		5000
#define LINUXU_WQ_SPAWN_RETRY_MS	50
#define LINUXU_WQ_WARN_LIMIT		32

static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;	/* barriers */
static pthread_cond_t manager_cv = PTHREAD_COND_INITIALIZER;

struct wq_worker {
	struct wq_worker *next;		/* zombie list */
	struct workqueue_struct *wq;
	pthread_t thread;
	bool extra;			/* counted in shared_workers */
	bool join_failed;
};

struct workqueue_struct {
	char name[64];
	unsigned int flags;
	int max_active;
	unsigned int max_workers;
	bool ordered;
	bool is_single;
	bool is_system;
	bool stop;			/* destroy: only chained work queues */
	bool linked;			/* on all_queues */
	bool reentry_wait;		/* a worker skipped an item running elsewhere */
	bool spawn_warned;
	pthread_cond_t cv;		/* workers and destroy wait here */
	struct list_head node;		/* all_queues */
	struct list_head queued;	/* FIFO of runnable items */
	unsigned int nr_queued;
	unsigned int nr_workers;	/* live threads, including starting ones */
	unsigned int nr_starting;	/* created, not yet in the worker loop */
	unsigned int nr_spawning;	/* manager is inside pthread_create */
	unsigned int nr_reaping;	/* manager is joining retired workers */
	unsigned int nr_idle;
	unsigned int nr_running;	/* callbacks in progress */
	unsigned long spawn_retry_at;
	struct wq_worker *zombies;	/* exited, not yet joined */
};

struct work_execution {
	struct work_execution *next;
	struct work_struct *work;
	struct workqueue_struct *wq;
	pthread_t thread;
	unsigned long ticket;
	bool cancelling;
};
struct work_barrier {
	struct work_barrier *next;
	unsigned long running, queued;
};
static struct work_execution *executions;
static struct work_barrier *barriers;
static unsigned long next_ticket;
static LIST_HEAD(all_queues);
static LIST_HEAD(delayed_works);	/* every timer-pending delayed_work */
static unsigned int shared_workers;
static pthread_t manager_thread;
static bool manager_started;
static bool system_queues_ready;
static unsigned int wq_warnings;
static unsigned int queue_flushers;	/* flush_workqueue() callers waiting */
static unsigned int worker_idle_ms = LINUXU_WQ_IDLE_MS;

/* ---- diagnostics ---- */

static void wq_warn(const char *fmt, ...)
{
	char text[256];
	va_list args;
	unsigned int n = __atomic_fetch_add(&wq_warnings, 1, __ATOMIC_RELAXED);

	if (n > LINUXU_WQ_WARN_LIMIT)
		return;
	if (n == LINUXU_WQ_WARN_LIMIT) {
		printk(KERN_ERR "linuxu workqueue: further warnings suppressed\n");
		return;
	}
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	printk(KERN_ERR "linuxu workqueue: %s\n", text);
}

/* ---- work state ---- */

static struct work_execution *running_work(struct work_struct *work)
{
	struct work_execution *execution;
	for (execution = executions; execution; execution = execution->next)
		if (execution->work == work) return execution;
	return NULL;
}

/* True when the caller is a worker currently executing an item of wq. */
static bool is_chained_locked(struct workqueue_struct *wq)
{
	struct work_execution *execution;
	pthread_t self = pthread_self();
	for (execution = executions; execution; execution = execution->next)
		if (execution->wq == wq && pthread_equal(execution->thread, self))
			return true;
	return false;
}

static void finish_ticket(unsigned long ticket)
{
	struct work_barrier *barrier;
	for (barrier = barriers; barrier; barrier = barrier->next) {
		if (barrier->running == ticket) barrier->running = 0;
		if (barrier->queued == ticket) barrier->queued = 0;
	}
	pthread_cond_broadcast(&work_cv);
}

static long state(const struct work_struct *work)
{
	return __atomic_load_n(&work->atomic_flags.counter, __ATOMIC_RELAXED);
}

static void set_state(struct work_struct *work, long value)
{
	__atomic_store_n(&work->atomic_flags.counter, value, __ATOMIC_RELEASE);
}

static struct delayed_work *as_delayed(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}

static unsigned long take_ticket(void)
{
	/* Zero denotes an absent barrier. Ticket reuse requires 2^64 queues. */
	if (++next_ticket == 0) ++next_ticket;
	return next_ticket;
}

static unsigned long ms_to_ticks(unsigned long ms)
{
	unsigned long ticks = (ms * HZ + 999) / 1000;
	return ticks ? ticks : 1;
}

/* The manager drops work_lock while it starts or joins threads.  The flag
 * records events from that window so it rescans instead of sleeping. */
static bool manager_kicked;
static void kick_manager(void)
{
	manager_kicked = true;
	pthread_cond_signal(&manager_cv);
}

static bool wants_worker(struct workqueue_struct *wq)
{
	if (wq->nr_queued <= wq->nr_idle + wq->nr_starting)
		return false;
	if (wq->nr_workers >= wq->max_workers)
		return false;
	if (wq->nr_workers && shared_workers >= LINUXU_WQ_SHARED_WORKERS)
		return false;
	return true;
}

static void wake_queue(struct workqueue_struct *wq)
{
	pthread_cond_broadcast(&wq->cv);
	if (wants_worker(wq))
		kick_manager();
}

/* Append to the runnable FIFO.  The caller has set work->wq/queue_seq. */
static void enqueue_locked(struct work_struct *work)
{
	struct workqueue_struct *wq = work->wq;

	list_add_tail(&work->entry, &wq->queued);
	wq->nr_queued++;
	wake_queue(wq);
}

static void remove_pending(struct work_struct *work, bool cancelled)
{
	long s = state(work);

	if (!(s & WORK_PENDING))
		return;
	if (s & WORK_DELAYED) {
		list_del_init(&as_delayed(work)->entry);
	} else {
		list_del_init(&work->entry);
		work->wq->nr_queued--;
	}
	set_state(work, s & ~(WORK_PENDING | WORK_DELAYED));
	if (cancelled) finish_ticket(work->queue_seq);
	pthread_cond_broadcast(&work->wq->cv);
	pthread_cond_broadcast(&work_cv);
}

static void add_pending(struct workqueue_struct *wq, struct work_struct *work,
			bool delayed, unsigned long expires)
{
	long s = state(work);

	work->wq = wq;
	work->queue_seq = take_ticket();
	if (delayed) {
		struct delayed_work *dw = as_delayed(work);
		dw->timer.expires = expires;
		list_add_tail(&dw->entry, &delayed_works);
		set_state(work, s | WORK_PENDING | WORK_DELAYED);
		kick_manager();
	} else {
		set_state(work, s | WORK_PENDING);
		enqueue_locked(work);
	}
	pthread_cond_broadcast(&work_cv);
}

/* Timer expiry: the item joins the tail of its queue like Linux's
 * delayed_work_timer_fn -> __queue_work.  A fresh ticket keeps
 * flush_workqueue() from waiting on items that became runnable later. */
static void promote_locked(struct work_struct *work)
{
	list_del_init(&as_delayed(work)->entry);
	set_state(work, state(work) & ~WORK_DELAYED);
	work->queue_seq = take_ticket();
	enqueue_locked(work);
}

/* Linux refuses non-chained submissions to a queue being destroyed
 * (__WQ_DESTROYING); a delayed submission could fire after the free. */
static bool stopping_refuses(struct workqueue_struct *wq, unsigned long delay,
			     const char *api, void *caller)
{
	if (!wq->stop)
		return false;
	if (!delay && is_chained_locked(wq))
		return false;
	wq_warn("%s() on workqueue \"%s\" during destroy_workqueue() refused "
		"(caller %p)", api, wq->name, caller);
	return true;
}

/* ---- workers ---- */

/* First runnable item.  An item still running elsewhere waits for that
 * execution to finish; an ordered queue never runs past its head. */
static struct work_struct *pick_work(struct workqueue_struct *wq)
{
	struct work_struct *work;

	list_for_each_entry(work, &wq->queued, entry) {
		if (!running_work(work))
			return work;
		wq->reentry_wait = true;
		if (wq->ordered)
			break;
	}
	return NULL;
}

static void wake_reentry_waiters(void)
{
	struct workqueue_struct *wq;
	list_for_each_entry(wq, &all_queues, node) {
		if (wq->reentry_wait) {
			wq->reentry_wait = false;
			pthread_cond_broadcast(&wq->cv);
		}
	}
}

static void run_work_locked(struct workqueue_struct *wq, struct work_struct *work)
{
	struct work_execution execution = {
		.next = executions, .work = work, .wq = wq,
		.thread = pthread_self(), .ticket = work->queue_seq,
	};
	work_func_t function = work->func;

	remove_pending(work, false);
	executions = &execution;
	wq->nr_running++;
	if (wants_worker(wq))
		kick_manager();
	pthread_mutex_unlock(&work_lock);
	function(work);
	pthread_mutex_lock(&work_lock);
	struct work_execution **link = &executions;
	while (*link != &execution) link = &(*link)->next;
	*link = execution.next;
	wq->nr_running--;
	finish_ticket(execution.ticket);
	pthread_cond_broadcast(&wq->cv);
	wake_reentry_waiters();
}

static void *worker_main(void *arg)
{
	struct wq_worker *self = arg;
	struct workqueue_struct *wq = self->wq;
	unsigned long idle_since = 0;
	bool idle_valid = false;

	pthread_mutex_lock(&work_lock);
	wq->nr_starting--;
	for (;;) {
		struct work_struct *work = pick_work(wq);
		unsigned long now, ticks, idle_ticks = ms_to_ticks(worker_idle_ms);

		if (work) {
			idle_valid = false;
			run_work_locked(wq, work);
			continue;
		}
		if (wq->stop && !wq->nr_queued)
			break;
		now = jiffies;
		if (!idle_valid) {
			idle_since = now;
			idle_valid = true;
		}
		if (!wq->nr_queued && time_after_eq(now, idle_since + idle_ticks))
			break;
		ticks = time_after(idle_since + idle_ticks, now) ?
			idle_since + idle_ticks - now : 1;
		struct timespec rel = {
			.tv_sec = ticks / HZ,
			.tv_nsec = (ticks % HZ) * (1000000000L / HZ),
		};
		wq->nr_idle++;
		pthread_cond_timedwait_relative_np(&wq->cv, &work_lock, &rel);
		wq->nr_idle--;
	}
	/* Retire.  The joiner owns self from here on. */
	wq->nr_workers--;
	if (self->extra)
		shared_workers--;
	self->next = wq->zombies;
	wq->zombies = self;
	pthread_cond_broadcast(&wq->cv);
	kick_manager();
	pthread_mutex_unlock(&work_lock);
	return NULL;
}

/* Called by the manager with work_lock held; drops it around thread
 * creation.  nr_spawning keeps the queue alive across the unlock. */
static int spawn_worker_locked(struct workqueue_struct *wq)
{
	struct wq_worker *worker = calloc(1, sizeof(*worker));
	int r;

	if (!worker)
		return ENOMEM;
	worker->wq = wq;
	worker->extra = wq->nr_workers > 0;
	wq->nr_workers++;
	wq->nr_starting++;
	wq->nr_spawning++;
	if (worker->extra)
		shared_workers++;
	pthread_mutex_unlock(&work_lock);
	r = pthread_create(&worker->thread, NULL, worker_main, worker);
	pthread_mutex_lock(&work_lock);
	wq->nr_spawning--;
	if (r) {
		wq->nr_workers--;
		wq->nr_starting--;
		if (worker->extra)
			shared_workers--;
		free(worker);
	}
	pthread_cond_broadcast(&wq->cv);
	return r;
}

/* Join retired workers of a live queue.  Returns true if it dropped the
 * lock (the caller must restart any queue iteration). */
static bool reap_locked(struct workqueue_struct *wq)
{
	struct wq_worker *list = NULL, **link = &wq->zombies;
	unsigned int count = 0;

	while (*link) {
		struct wq_worker *worker = *link;
		if (worker->join_failed) {
			link = &worker->next;
			continue;
		}
		*link = worker->next;
		worker->next = list;
		list = worker;
		count++;
	}
	if (!count)
		return false;
	wq->nr_reaping += count;
	pthread_mutex_unlock(&work_lock);
	while (list) {
		struct wq_worker *worker = list;
		list = worker->next;
		if (pthread_join(worker->thread, NULL)) {
			/* Completion is uncertain: keep the record, never retry here. */
			pthread_mutex_lock(&work_lock);
			worker->join_failed = true;
			worker->next = wq->zombies;
			wq->zombies = worker;
			pthread_mutex_unlock(&work_lock);
			wq_warn("could not join a retired worker of \"%s\"", wq->name);
		} else {
			free(worker);
		}
	}
	pthread_mutex_lock(&work_lock);
	wq->nr_reaping -= count;
	pthread_cond_broadcast(&wq->cv);
	return true;
}

static void *manager_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&work_lock);
	for (;;) {
		struct workqueue_struct *wq;
		struct delayed_work *dw, *next;
		unsigned long now = jiffies, wake = 0;
		bool has_wake = false;

		manager_kicked = false;
		list_for_each_entry_safe(dw, next, &delayed_works, entry) {
			if (time_after_eq(now, dw->timer.expires)) {
				promote_locked(&dw->work);
			} else if (!has_wake || time_before(dw->timer.expires, wake)) {
				wake = dw->timer.expires;
				has_wake = true;
			}
		}
rescan:
		list_for_each_entry(wq, &all_queues, node) {
			if (wants_worker(wq)) {
				if (wq->spawn_retry_at &&
				    time_before(jiffies, wq->spawn_retry_at)) {
					if (!has_wake || time_before(wq->spawn_retry_at, wake)) {
						wake = wq->spawn_retry_at;
						has_wake = true;
					}
					continue;
				}
				int r = spawn_worker_locked(wq);
				if (r) {
					wq->spawn_retry_at = jiffies +
						ms_to_ticks(LINUXU_WQ_SPAWN_RETRY_MS);
					if (!wq->spawn_warned) {
						wq->spawn_warned = true;
						wq_warn("cannot start a worker for \"%s\" "
							"(error %d); %u item(s) wait, retrying",
							wq->name, r, wq->nr_queued);
					}
				} else {
					wq->spawn_retry_at = 0;
					wq->spawn_warned = false;
				}
				goto rescan;
			}
			if (!wq->stop && wq->zombies && reap_locked(wq))
				goto rescan;
		}
		if (manager_kicked)
			continue;
		if (has_wake) {
			now = jiffies;
			unsigned long ticks = time_after(wake, now) ? wake - now : 0;
			if (!ticks)
				continue;
			struct timespec rel = {
				.tv_sec = ticks / HZ,
				.tv_nsec = (ticks % HZ) * (1000000000L / HZ),
			};
			pthread_cond_timedwait_relative_np(&manager_cv, &work_lock, &rel);
		} else {
			pthread_cond_wait(&manager_cv, &work_lock);
		}
	}
	/* Not reached: the manager lives for the process. */
	pthread_mutex_unlock(&work_lock);
	return NULL;
}

/* ---- queue lifetime ---- */

static struct workqueue_struct *new_queue(const char *name, unsigned int flags,
					  int max_active, bool ordered)
{
	struct workqueue_struct *wq = calloc(1, sizeof(*wq));

	if (!wq)
		return NULL;
	if (name)
		snprintf(wq->name, sizeof(wq->name), "%s", name);
	wq->flags = flags | (ordered ? __WQ_ORDERED : 0);
	wq->max_active = max_active;
	if (pthread_cond_init(&wq->cv, NULL)) {
		free(wq);
		return NULL;
	}
	INIT_LIST_HEAD(&wq->node);
	INIT_LIST_HEAD(&wq->queued);
	wq->ordered = ordered;
	wq->is_single = ordered;
	if (ordered) {
		wq->max_workers = 1;
	} else {
		int limit = max_active > 0 ? max_active : WQ_DFL_ACTIVE;
		wq->max_workers = limit > LINUXU_WQ_MAX_WORKERS ?
			LINUXU_WQ_MAX_WORKERS : (unsigned int)limit;
	}
	return wq;
}

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_unbound_highpri_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_power_wq;
struct workqueue_struct *system_switch_wq;
struct workqueue_struct *system_freezable_highpri_wq;
struct workqueue_struct *system_dfl_wq;
struct workqueue_struct *system_unbound_dfl_wq;

static const struct {
	struct workqueue_struct **slot;
	const char *name;
	unsigned int flags;
} system_queues[] = {
	{ &system_wq,			"events",			0 },
	{ &system_highpri_wq,		"events_highpri",		WQ_HIGHPRI },
	{ &system_long_wq,		"events_long",			0 },
	{ &system_unbound_wq,		"events_unbound",		WQ_UNBOUND },
	{ &system_unbound_highpri_wq,	"events_unbound_highpri",	WQ_UNBOUND | WQ_HIGHPRI },
	{ &system_freezable_wq,		"events_freezable",		WQ_FREEZABLE },
	{ &system_power_wq,		"events_power_efficient",	0 },
	{ &system_switch_wq,		"events_switch",		0 },
	{ &system_freezable_highpri_wq,	"events_freezable_highpri",	WQ_FREEZABLE | WQ_HIGHPRI },
	{ &system_dfl_wq,		"events_dfl",			WQ_UNBOUND },
	{ &system_unbound_dfl_wq,	"events_unbound_dfl",		WQ_UNBOUND },
};

static int workqueue_init_locked(void)
{
	size_t i;

	/* The manager starts first: a published system queue must be able to
	 * get workers.  It waits for work_lock before touching any state. */
	if (!manager_started) {
		if (pthread_create(&manager_thread, NULL, manager_main, NULL))
			return -EAGAIN;
		manager_started = true;
	}
	if (!system_queues_ready) {
		for (i = 0; i < sizeof(system_queues) / sizeof(system_queues[0]); i++) {
			struct workqueue_struct *wq;
			if (*system_queues[i].slot)
				continue;
			wq = new_queue(system_queues[i].name, system_queues[i].flags,
				       0, false);
			if (!wq)
				return -ENOMEM;
			wq->is_system = true;
			wq->linked = true;
			list_add_tail(&wq->node, &all_queues);
			__atomic_store_n(system_queues[i].slot, wq, __ATOMIC_RELEASE);
		}
		system_queues_ready = true;
	}
	return 0;
}

/* Create the system queues and the manager thread.  Idempotent and
 * thread-safe; a failed call may be retried. */
int linuxu_workqueue_init(void)
{
	int ret;

	pthread_mutex_lock(&work_lock);
	ret = workqueue_init_locked();
	pthread_mutex_unlock(&work_lock);
	return ret;
}

struct workqueue_struct *linuxu_default_wq(void)
{
	struct workqueue_struct *wq = __atomic_load_n(&system_wq, __ATOMIC_ACQUIRE);

	if (wq || linuxu_workqueue_init())
		return wq;
	return __atomic_load_n(&system_wq, __ATOMIC_ACQUIRE);
}

static struct workqueue_struct *queue_create(const char *name, unsigned int flags,
					     int max_active, bool ordered)
{
	struct workqueue_struct *wq;

	/* queue_work() relies on the manager to start workers. */
	if (linuxu_workqueue_init())
		return NULL;
	wq = new_queue(name, flags, max_active, ordered);
	if (!wq)
		return NULL;
	pthread_mutex_lock(&work_lock);
	wq->linked = true;
	list_add_tail(&wq->node, &all_queues);
	pthread_mutex_unlock(&work_lock);
	return wq;
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
					 int max_active)
{
	/* An unbound max_active == 1 queue is the historical ordered form. */
	return queue_create(name, flags, max_active,
			    (flags & __WQ_ORDERED) || max_active == 1);
}

struct workqueue_struct *create_workqueue(const char *name)
{
	return queue_create(name, 0, 0, false);
}

struct workqueue_struct *create_singlethread_workqueue(const char *name)
{
	return queue_create(name, WQ_MEM_RECLAIM, 1, true);
}

struct workqueue_struct *alloc_ordered_workqueue(const char *name,
					 unsigned int flags, int max_active)
{
	(void)max_active;
	return queue_create(name, flags | WQ_UNBOUND, 1, true);
}

/* A NULL queue is a caller bug (or a system queue read before
 * linuxu_workqueue_init()).  Dropping the work would hang its waiters, so
 * report it and run the work on system_wq. */
static struct workqueue_struct *resolve_queue(struct workqueue_struct *wq,
					      const char *api, void *caller)
{
	if (wq)
		return wq;
	wq_warn("%s() called with a NULL workqueue (caller %p); running the "
		"work on system_wq instead of dropping it", api, caller);
	return linuxu_default_wq();
}

/* ---- submission ---- */

static bool queue_work_caller(struct workqueue_struct *wq,
			      struct work_struct *work, void *caller)
{
	bool queued = false;

	wq = resolve_queue(wq, "queue_work", caller);
	if (!wq || !work || !work->func)
		return false;
	pthread_mutex_lock(&work_lock);
	struct work_execution *execution = running_work(work);
	if (!(state(work) & WORK_PENDING) &&
	    !(execution && execution->cancelling) &&
	    !stopping_refuses(wq, 0, "queue_work", caller)) {
		add_pending(wq, work, false, 0);
		queued = true;
	}
	pthread_mutex_unlock(&work_lock);
	return queued;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	return queue_work_caller(wq, work, __builtin_return_address(0));
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	(void)cpu;
	return queue_work_caller(wq, work, __builtin_return_address(0));
}

bool queue_work_node(int node, struct workqueue_struct *wq, struct work_struct *work)
{
	(void)node;
	return queue_work_caller(wq, work, __builtin_return_address(0));
}

bool schedule_work(struct work_struct *work)
{
	return queue_work_caller(linuxu_default_wq(), work,
				 __builtin_return_address(0));
}

static bool queue_delayed_caller(struct workqueue_struct *wq,
				 struct delayed_work *dwork, unsigned long delay,
				 void *caller)
{
	bool queued = false;

	wq = resolve_queue(wq, "queue_delayed_work", caller);
	if (!wq || !dwork || !dwork->work.func)
		return false;
	pthread_mutex_lock(&work_lock);
	struct work_execution *execution = running_work(&dwork->work);
	if (!(state(&dwork->work) & WORK_PENDING) &&
	    !(execution && execution->cancelling) &&
	    !stopping_refuses(wq, delay, "queue_delayed_work", caller)) {
		add_pending(wq, &dwork->work, delay != 0, jiffies + delay);
		queued = true;
	}
	pthread_mutex_unlock(&work_lock);
	return queued;
}

bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay)
{
	return queue_delayed_caller(wq, dwork, delay, __builtin_return_address(0));
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			   struct delayed_work *dw, unsigned long delay)
{
	(void)cpu;
	return queue_delayed_caller(wq, dw, delay, __builtin_return_address(0));
}

bool schedule_delayed_work(struct delayed_work *dw, unsigned long delay)
{
	return queue_delayed_caller(linuxu_default_wq(), dw, delay,
				    __builtin_return_address(0));
}

/* Linux mod_delayed_work_on(): steal a pending instance (its flush_work()
 * barrier completes, as when the pwq barrier is reached) and re-arm. */
static bool mod_delayed_caller(struct workqueue_struct *wq,
			       struct delayed_work *dw, unsigned long delay,
			       void *caller)
{
	bool was = false;
	struct work_struct *work = &dw->work;

	wq = resolve_queue(wq, "mod_delayed_work", caller);
	if (!wq || !dw || !work->func)
		return false;
	pthread_mutex_lock(&work_lock);
	struct work_execution *execution = running_work(work);
	if (!(execution && execution->cancelling) &&
	    !stopping_refuses(wq, delay, "mod_delayed_work", caller)) {
		was = (state(work) & WORK_PENDING) != 0;
		if (was)
			remove_pending(work, true);
		add_pending(wq, work, delay != 0, jiffies + delay);
	}
	pthread_mutex_unlock(&work_lock);
	return was;
}

bool mod_delayed_work(struct workqueue_struct *wq,
		      struct delayed_work *dw, unsigned long delay)
{
	return mod_delayed_caller(wq, dw, delay, __builtin_return_address(0));
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
			 struct delayed_work *dw, unsigned long delay)
{
	(void)cpu;
	return mod_delayed_caller(wq, dw, delay, __builtin_return_address(0));
}

bool work_pending(struct work_struct *work)
{
	return (state(work) & WORK_PENDING) != 0;
}

unsigned int work_busy(struct work_struct *work)
{
	unsigned int busy;
	pthread_mutex_lock(&work_lock);
	busy = (state(work) & WORK_PENDING ? WORK_BUSY_PENDING : 0) |
	       (running_work(work) ? WORK_BUSY_RUNNING : 0);
	pthread_mutex_unlock(&work_lock);
	return busy;
}

/* ---- barriers ---- */

/* Wait for the instance queued at the call (a timer-pending delayed item
 * has no queued instance yet, as in Linux) and the running instance. */
static bool flush_work_locked(struct work_struct *work, void *caller)
{
	struct work_barrier barrier;
	struct work_execution *execution;
	long s = state(work);

	execution = running_work(work);
	barrier.running = execution ? execution->ticket : 0;
	barrier.queued = (s & WORK_PENDING) && !(s & WORK_DELAYED) ?
		work->queue_seq : 0;
	if (execution && pthread_equal(execution->thread, pthread_self())) {
		wq_warn("work %p flushes itself from its own callback (caller %p)",
			(void *)work, caller);
		barrier.running = 0;
		if (barrier.queued) {
			/* The queued instance cannot start before we return. */
			barrier.queued = 0;
		}
	}
	bool busy = barrier.running || barrier.queued;
	barrier.next = barriers;
	barriers = &barrier;
	while (barrier.running || barrier.queued)
		pthread_cond_wait(&work_cv, &work_lock);
	struct work_barrier **link = &barriers;
	while (*link != &barrier) link = &(*link)->next;
	*link = barrier.next;
	return busy;
}

bool flush_work(struct work_struct *work)
{
	void *caller = __builtin_return_address(0);
	pthread_mutex_lock(&work_lock);
	bool busy = flush_work_locked(work, caller);
	pthread_mutex_unlock(&work_lock);
	return busy;
}

bool flush_delayed_work(struct delayed_work *dw)
{
	struct work_struct *work = &dw->work;
	void *caller = __builtin_return_address(0);

	pthread_mutex_lock(&work_lock);
	if ((state(work) & (WORK_PENDING | WORK_DELAYED)) ==
	    (WORK_PENDING | WORK_DELAYED))
		promote_locked(work);
	bool busy = flush_work_locked(work, caller);
	pthread_mutex_unlock(&work_lock);
	return busy;
}

/* Does wq still hold work queued or running with a ticket at or before
 * limit?  The caller's own execution is excluded (it cannot finish while
 * the caller waits). */
static bool queue_busy_before(struct workqueue_struct *wq, unsigned long limit,
			      bool *self_flush)
{
	struct work_struct *work;
	struct work_execution *execution;
	pthread_t self = pthread_self();

	list_for_each_entry(work, &wq->queued, entry)
		if (work->queue_seq <= limit)
			return true;
	for (execution = executions; execution; execution = execution->next) {
		if (execution->wq != wq || execution->ticket > limit)
			continue;
		if (pthread_equal(execution->thread, self)) {
			*self_flush = true;
			continue;
		}
		return true;
	}
	return false;
}

void flush_workqueue(struct workqueue_struct *wq)
{
	bool self_flush = false;

	if (!wq) {
		wq_warn("flush_workqueue(NULL) (caller %p)",
			__builtin_return_address(0));
		return;
	}
	pthread_mutex_lock(&work_lock);
	/* Everything queued before this point carries a ticket <= limit;
	 * later submissions, requeues and timer expiries get larger ones. */
	unsigned long limit = next_ticket;
	queue_flushers++;
	while (queue_busy_before(wq, limit, &self_flush))
		pthread_cond_wait(&work_cv, &work_lock);
	queue_flushers--;
	pthread_mutex_unlock(&work_lock);
	if (self_flush)
		wq_warn("flush_workqueue(\"%s\") from one of its own work items",
			wq->name);
}

/* Wait until nothing is queued or running, including chained requeues.
 * Like Linux, timer-pending delayed work is not waited for. */
static void drain_locked(struct workqueue_struct *wq)
{
	for (;;) {
		bool self_flush = false;
		if (!queue_busy_before(wq, ~0UL, &self_flush))
			break;
		pthread_cond_wait(&work_cv, &work_lock);
	}
}

void drain_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;
	pthread_mutex_lock(&work_lock);
	drain_locked(wq);
	pthread_mutex_unlock(&work_lock);
}

static bool cancel_sync(struct work_struct *work, void *caller)
{
	struct work_execution *execution;
	bool was;

	pthread_mutex_lock(&work_lock);
	execution = running_work(work);
	was = (state(work) & WORK_PENDING) != 0;
	remove_pending(work, true);
	if (execution && pthread_equal(execution->thread, pthread_self())) {
		/* Waiting for ourselves would never return. */
		wq_warn("work %p cancels itself synchronously (caller %p)",
			(void *)work, caller);
	} else if (execution) {
		execution->cancelling = true;
		while (running_work(work))
			pthread_cond_wait(&work_cv, &work_lock);
	}
	pthread_mutex_unlock(&work_lock);
	return was;
}

bool cancel_work_sync(struct work_struct *work)
{
	return cancel_sync(work, __builtin_return_address(0));
}

bool cancel_delayed_work_sync(struct delayed_work *dw)
{
	return cancel_sync(&dw->work, __builtin_return_address(0));
}

bool cancel_work(struct work_struct *work)
{
	bool was;

	pthread_mutex_lock(&work_lock);
	was = !!(state(work) & WORK_PENDING);
	if (was)
		remove_pending(work, true);
	pthread_mutex_unlock(&work_lock);
	return was;
}

bool cancel_delayed_work(struct delayed_work *dw)
{
	return cancel_work(&dw->work);
}

int workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	int busy = 0;
	(void)cpu;
	if (!wq)
		return 0;
	pthread_mutex_lock(&work_lock);
	busy = wq->nr_queued > wq->nr_idle;
	pthread_mutex_unlock(&work_lock);
	return busy;
}

/* Linux destroy_workqueue(): mark the queue destroying, drain queued and
 * running work (chained requeues from its own items still run), then free.
 * Pending delayed work is a caller bug there: its timer fires into a
 * destroying queue, is refused with a WARN, or touches freed memory.  Here
 * it is cancelled with a warning so nothing can reference the freed queue;
 * it is never run early. */
void destroy_workqueue(struct workqueue_struct *wq)
{
	struct delayed_work *dw, *next;

	if (!wq)
		return;
	pthread_mutex_lock(&work_lock);
	if (wq->is_system) {
		pthread_mutex_unlock(&work_lock);
		wq_warn("destroy_workqueue() on system queue \"%s\" ignored", wq->name);
		return;
	}
	wq->stop = true;
	list_for_each_entry_safe(dw, next, &delayed_works, entry) {
		if (dw->work.wq != wq)
			continue;
		wq_warn("destroy_workqueue(\"%s\"): delayed work %p (fn %p) still "
			"pending; cancelled", wq->name, (void *)dw,
			(void *)dw->work.func);
		remove_pending(&dw->work, true);
	}
	drain_locked(wq);
	pthread_cond_broadcast(&wq->cv);
	while (wq->nr_workers || wq->nr_spawning || wq->nr_reaping)
		pthread_cond_wait(&wq->cv, &work_lock);
	if (wq->linked) {
		list_del_init(&wq->node);
		wq->linked = false;
	}
	while (wq->zombies) {
		struct wq_worker *worker = wq->zombies;
		wq->zombies = worker->next;
		pthread_mutex_unlock(&work_lock);
		int r = pthread_join(worker->thread, NULL);
		pthread_mutex_lock(&work_lock);
		if (r) {
			/* Completion is uncertain: preserve the worker's storage. */
			worker->next = wq->zombies;
			wq->zombies = worker;
			pthread_mutex_unlock(&work_lock);
			return;
		}
		free(worker);
	}
	pthread_mutex_unlock(&work_lock);
	if (pthread_cond_destroy(&wq->cv)) return;
	free(wq);
}

void workqueue_flush(int cpu) { (void)cpu; }
bool workqueue_is_single_threaded(struct workqueue_struct *wq)
{
	return wq && wq->is_single;
}
