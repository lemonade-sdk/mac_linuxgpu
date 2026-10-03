/* Linux timer_list adapter: absolute HZ=100 jiffies deadlines.  The
 * timer's hlist node is its pending state; no fixed-size side table. */
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>

#include <linux/timer.h>
#include <linux/jiffies.h>

static struct hlist_head timers = HLIST_HEAD_INIT;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cv = PTHREAD_COND_INITIALIZER;
static pthread_t timer_worker;
static bool timer_worker_started;
static struct timer_list *running_timer;

static void *timer_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&timer_lock);
	for (;;) {
		struct timer_list *timer = NULL;
		struct hlist_node *node;
		unsigned long earliest = 0;
		unsigned long now = jiffies;

		hlist_for_each(node, &timers) {
			struct timer_list *current =
				hlist_entry(node, struct timer_list, entry);
			if (time_after_eq(now, current->expires)) {
				timer = current;
				break;
			}
			if (!earliest || time_before(current->expires, earliest))
				earliest = current->expires;
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
		if (earliest) {
			unsigned long ticks = earliest - now;
			struct timespec rel = {
				.tv_sec = ticks / HZ,
				.tv_nsec = (ticks % HZ) * (1000000000L / HZ),
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
