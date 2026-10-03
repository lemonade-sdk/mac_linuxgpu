/* Linux timer service: timer_list (absolute HZ=100 jiffies deadlines) and
 * hrtimer (nanosecond deadlines on CLOCK_MONOTONIC, CLOCK_REALTIME or
 * CLOCK_BOOTTIME). One worker thread runs both kinds of callback in expiry
 * order, as the timer softirq and the hrtimer interrupt would. Each timer's
 * hlist node is its pending state; no fixed-size side table. */
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/bug.h>

static struct hlist_head timers = HLIST_HEAD_INIT;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cv = PTHREAD_COND_INITIALIZER;
static pthread_t timer_worker;
static bool timer_worker_started;
static struct timer_list *running_timer;
static struct hlist_head hrtimers = HLIST_HEAD_INIT;
static struct hrtimer *running_hrtimer;

static ktime_t hrtimer_clock_now(clockid_t clock)
{
	switch (clock) {
	case CLOCK_REALTIME:
		return ktime_get_real();
#ifdef CLOCK_BOOTTIME
	case CLOCK_BOOTTIME:
		return ktime_get_boottime();
#endif
	default:
		return ktime_get();
	}
}

/* Nanoseconds until timer's (soft) expiry; <= 0 when due. */
static s64 hrtimer_due_in(const struct hrtimer *timer)
{
	return ktime_to_ns(ktime_sub(timer->_softexpires,
				     hrtimer_clock_now(timer->linuxu_clock)));
}

/* Called with timer_lock held. */
static void hrtimer_dequeue(struct hrtimer *timer)
{
	if (timer->is_queued) {
		hlist_del_init(&timer->linuxu_entry);
		timer->is_queued = false;
	}
}

static void hrtimer_enqueue(struct hrtimer *timer)
{
	if (!timer->is_queued) {
		hlist_add_head(&timer->linuxu_entry, &hrtimers);
		timer->is_queued = true;
	}
	pthread_cond_broadcast(&timer_cv);
}

/* Run one due hrtimer with timer_lock held across the bookkeeping only.
 * Linux __run_hrtimer(): the timer is dequeued before the callback, and a
 * HRTIMER_RESTART return re-enqueues it at the expiry the callback set
 * (unless the callback already restarted it). After a NORESTART return the
 * timer is not touched: its owner may free it from the callback. */
static void run_hrtimer_locked(struct hrtimer *timer)
{
	enum hrtimer_restart (*function)(struct hrtimer *) = timer->function;
	enum hrtimer_restart restart = HRTIMER_NORESTART;

	hrtimer_dequeue(timer);
	running_hrtimer = timer;
	pthread_mutex_unlock(&timer_lock);
	if (function)
		restart = function(timer);
	pthread_mutex_lock(&timer_lock);
	if (restart == HRTIMER_RESTART)
		hrtimer_enqueue(timer);
	running_hrtimer = NULL;
	pthread_cond_broadcast(&timer_cv);
}

static void *timer_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&timer_lock);
	for (;;) {
		struct timer_list *timer = NULL;
		struct hrtimer *hrtimer = NULL;
		struct hlist_node *node;
		unsigned long earliest = 0;
		unsigned long now = jiffies;
		s64 hr_wait = -1;

		hlist_for_each(node, &hrtimers) {
			struct hrtimer *cur =
				hlist_entry(node, struct hrtimer, linuxu_entry);
			s64 due = hrtimer_due_in(cur);
			if (due <= 0) {
				hrtimer = cur;
				break;
			}
			if (hr_wait < 0 || due < hr_wait)
				hr_wait = due;
		}
		if (hrtimer) {
			run_hrtimer_locked(hrtimer);
			continue;
		}

		hlist_for_each(node, &timers) {
			struct timer_list *cur =
				hlist_entry(node, struct timer_list, entry);
			if (time_after_eq(now, cur->expires)) {
				timer = cur;
				break;
			}
			if (!earliest || time_before(cur->expires, earliest))
				earliest = cur->expires;
		}
		if (timer) {
			hlist_del_init(&timer->entry);
			running_timer = timer;
			pthread_mutex_unlock(&timer_lock);
			if (timer->function)
				timer->function(timer);
			pthread_mutex_lock(&timer_lock);
			running_timer = NULL;
			pthread_cond_broadcast(&timer_cv);
			continue;
		}
		if (earliest || hr_wait >= 0) {
			s64 wait_ns = earliest ? (s64)(earliest - now) * (NSEC_PER_SEC / HZ) : -1;
			if (hr_wait >= 0 && (wait_ns < 0 || hr_wait < wait_ns))
				wait_ns = hr_wait;
			struct timespec rel = {
				.tv_sec = wait_ns / NSEC_PER_SEC,
				.tv_nsec = wait_ns % NSEC_PER_SEC,
			};
			pthread_cond_timedwait_relative_np(&timer_cv, &timer_lock, &rel);
		} else {
			pthread_cond_wait(&timer_cv, &timer_lock);
		}
	}
	/* Not reached: the kernel timer service lives for process lifetime. */
	pthread_mutex_unlock(&timer_lock);
	return NULL;
}

static bool ensure_worker(void)
{
	if (!timer_worker_started) {
		if (pthread_create(&timer_worker, NULL, timer_main, NULL))
			return false;
		timer_worker_started = true;
	}
	return true;
}

int linuxu_timer_service_init(void)
{
	pthread_mutex_lock(&timer_lock);
	int result = ensure_worker() ? 0 : -EAGAIN;
	pthread_mutex_unlock(&timer_lock);
	return result;
}

int timer_pending(const struct timer_list *timer)
{
	int pending;
	pthread_mutex_lock(&timer_lock);
	pending = !hlist_unhashed(&timer->entry);
	pthread_mutex_unlock(&timer_lock);
	return pending;
}

void add_timer(struct timer_list *timer)
{
	pthread_mutex_lock(&timer_lock);
	if (!timer->shutdown && !timer->deleting && ensure_worker() &&
	    hlist_unhashed(&timer->entry)) {
		if (!timer->expires)
			timer->expires = jiffies + 1;
		hlist_add_head(&timer->entry, &timers);
		pthread_cond_broadcast(&timer_cv);
	}
	pthread_mutex_unlock(&timer_lock);
}

void add_timer_on(struct timer_list *timer, int cpu)
{
	(void)cpu;
	add_timer(timer);
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
	int was;
	pthread_mutex_lock(&timer_lock);
	was = !hlist_unhashed(&timer->entry);
	if (!timer->shutdown && !timer->deleting && ensure_worker()) {
		timer->expires = expires;
		if (!was)
			hlist_add_head(&timer->entry, &timers);
		pthread_cond_broadcast(&timer_cv);
	}
	pthread_mutex_unlock(&timer_lock);
	return was;
}

int mod_timer_pending(struct timer_list *timer, unsigned long expires)
{
	int was;
	pthread_mutex_lock(&timer_lock);
	was = !hlist_unhashed(&timer->entry);
	if (was && !timer->shutdown && !timer->deleting) {
		timer->expires = expires;
		pthread_cond_broadcast(&timer_cv);
	}
	pthread_mutex_unlock(&timer_lock);
	return was;
}

int mod_timer_on(struct timer_list *timer, unsigned long expires, int cpu)
{
	(void)cpu;
	return mod_timer(timer, expires);
}

int del_timer(struct timer_list *timer)
{
	int was;
	pthread_mutex_lock(&timer_lock);
	was = !hlist_unhashed(&timer->entry);
	if (was)
		hlist_del_init(&timer->entry);
	pthread_cond_broadcast(&timer_cv);
	pthread_mutex_unlock(&timer_lock);
	return was;
}

int del_timer_sync(struct timer_list *timer)
{
	int was;
	pthread_mutex_lock(&timer_lock);
	timer->deleting++;
	was = !hlist_unhashed(&timer->entry);
	if (was)
		hlist_del_init(&timer->entry);
	/* A callback may rearm itself before it returns.  Remove that arm
	 * too before allowing the caller to release the timer storage. */
	while (running_timer == timer) {
		pthread_cond_wait(&timer_cv, &timer_lock);
	}
	if (!hlist_unhashed(&timer->entry)) {
		hlist_del_init(&timer->entry);
		was = 1;
	}
	timer->deleting--;
	pthread_cond_broadcast(&timer_cv);
	pthread_mutex_unlock(&timer_lock);
	return was;
}

bool timer_delete(struct timer_list *timer) { return del_timer(timer) != 0; }
bool timer_delete_sync(struct timer_list *timer)
{
	return del_timer_sync(timer) != 0;
}

void timer_start(struct timer_list *timer, unsigned long expires)
{
	mod_timer(timer, expires);
}

void timer_start_on(struct timer_list *timer, unsigned long expires, int cpu)
{
	(void)cpu;
	timer_start(timer, expires);
}

int timer_reduce(struct timer_list *timer, unsigned long expires)
{
	pthread_mutex_lock(&timer_lock);
	int was = !hlist_unhashed(&timer->entry);
	if (!timer->shutdown && !timer->deleting &&
	    (hlist_unhashed(&timer->entry) ||
	     time_before(expires, timer->expires))) {
		timer->expires = expires;
		if (hlist_unhashed(&timer->entry) && ensure_worker())
			hlist_add_head(&timer->entry, &timers);
		pthread_cond_broadcast(&timer_cv);
	}
	pthread_mutex_unlock(&timer_lock);
	return was;
}

void timer_destroy(struct timer_list *timer)
{
	timer_shutdown_sync(timer);
}

int timer_delete_sync_try(struct timer_list *timer)
{
	pthread_mutex_lock(&timer_lock);
	int result = running_timer == timer ? -1 : !hlist_unhashed(&timer->entry);
	if (result > 0) hlist_del_init(&timer->entry);
	pthread_mutex_unlock(&timer_lock);
	return result;
}
int timer_shutdown(struct timer_list *timer)
{
	pthread_mutex_lock(&timer_lock);
	timer->shutdown = true;
	int pending = !hlist_unhashed(&timer->entry);
	if (pending) hlist_del_init(&timer->entry);
	pthread_cond_broadcast(&timer_cv);
	pthread_mutex_unlock(&timer_lock);
	return pending;
}
int timer_shutdown_sync(struct timer_list *timer)
{
	int pending = timer_shutdown(timer);
	return del_timer_sync(timer) || pending;
}

/* ---- hrtimer ---- */

void hrtimer_setup(struct hrtimer *timer,
		   enum hrtimer_restart (*function)(struct hrtimer *),
		   clockid_t clock_id, enum hrtimer_mode mode)
{
	*timer = (struct hrtimer){
		.function = function,
		.linuxu_clock = clock_id,
		.is_rel = (mode & HRTIMER_MODE_REL) != 0,
		.is_soft = (mode & HRTIMER_MODE_SOFT) != 0,
		.is_hard = (mode & HRTIMER_MODE_HARD) != 0,
	};
	INIT_HLIST_NODE(&timer->linuxu_entry);
}

void hrtimer_setup_on_stack(struct hrtimer *timer,
			    enum hrtimer_restart (*function)(struct hrtimer *),
			    clockid_t clock_id, enum hrtimer_mode mode)
{
	hrtimer_setup(timer, function, clock_id, mode);
}

void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim, u64 range_ns,
			    const enum hrtimer_mode mode)
{
	pthread_mutex_lock(&timer_lock);
	if (!ensure_worker()) {
		/* linuxu_driver_bootstrap() starts the worker before any
		 * driver code runs; a failure here cannot be reported. */
		pthread_mutex_unlock(&timer_lock);
		WARN_ON_ONCE(1);
		return;
	}
	hrtimer_dequeue(timer);
	if (mode & HRTIMER_MODE_REL)
		tim = ktime_add_safe(hrtimer_clock_now(timer->linuxu_clock), tim);
	hrtimer_set_expires_range_ns(timer, tim, range_ns);
	hrtimer_enqueue(timer);
	pthread_mutex_unlock(&timer_lock);
}

/* 1: dequeued a pending timer; 0: not active; -1: callback running. */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	int ret;

	pthread_mutex_lock(&timer_lock);
	if (running_hrtimer == timer) {
		ret = -1;
	} else {
		ret = timer->is_queued ? 1 : 0;
		hrtimer_dequeue(timer);
	}
	pthread_mutex_unlock(&timer_lock);
	return ret;
}

/* Linux hrtimer_cancel(): wait for a running callback (which may restart
 * the timer), then dequeue. Returns 1 if the timer was pending. */
int hrtimer_cancel(struct hrtimer *timer)
{
	int ret;

	pthread_mutex_lock(&timer_lock);
	while (running_hrtimer == timer) {
		if (timer_worker_started &&
		    pthread_equal(pthread_self(), timer_worker)) {
			/* Cancelling a timer from its own callback deadlocks on
			 * Linux; report it and dequeue without waiting. */
			WARN_ON_ONCE(1);
			break;
		}
		pthread_cond_wait(&timer_cv, &timer_lock);
	}
	ret = timer->is_queued ? 1 : 0;
	hrtimer_dequeue(timer);
	pthread_mutex_unlock(&timer_lock);
	return ret;
}

bool hrtimer_active(const struct hrtimer *timer)
{
	bool active;

	pthread_mutex_lock(&timer_lock);
	active = timer->is_queued || running_hrtimer == timer;
	pthread_mutex_unlock(&timer_lock);
	return active;
}

ktime_t hrtimer_cb_get_time(const struct hrtimer *timer)
{
	return hrtimer_clock_now(timer->linuxu_clock);
}

ktime_t __hrtimer_get_remaining(const struct hrtimer *timer, bool adjust)
{
	(void)adjust;
	return ktime_sub(hrtimer_get_expires(timer),
			 hrtimer_clock_now(timer->linuxu_clock));
}

/* Linux hrtimer_forward(): move the expiry forward by whole intervals past
 * now and return the number of intervals (overruns). The resolution is
 * 1 ns, as with high-resolution timers. */
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta;

	delta = ktime_sub(now, hrtimer_get_expires(timer));
	if (delta < 0)
		return 0;
	if (WARN_ON_ONCE(timer->is_queued))
		return 0;
	if (interval < 1)
		interval = 1;
	if (delta >= interval) {
		s64 incr = ktime_to_ns(interval);

		orun = (u64)(ktime_to_ns(delta) / incr);
		hrtimer_add_expires_ns(timer, incr * orun);
		if (hrtimer_get_expires(timer) > now)
			return orun;
		/* This (and the ktime_add() below) is the correction for
		 * exact multiples of the interval. */
		orun++;
	}
	hrtimer_add_expires(timer, interval);
	return orun;
}
