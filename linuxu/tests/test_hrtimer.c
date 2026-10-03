/* hrtimer on the linuxu timer service (linuxu/src/timer.c): one-shot and
 * periodic (HRTIMER_RESTART + hrtimer_forward_now) timers, the Linux
 * cancel contracts while a callback runs, hrtimer_forward() overrun
 * arithmetic, the drm_vblank "while (hrtimer_active()) try_to_cancel()"
 * idiom, and timer_list timers sharing the worker. */
#include <assert.h>
#include <pthread.h>
#define T_LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define T_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define T_ADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#include <stdio.h>
#include <unistd.h>

#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

int linuxu_timer_service_init(void);

static int oneshot_fired;
static enum hrtimer_restart oneshot(struct hrtimer *t)
{
	(void)t;
	T_ADD(&oneshot_fired, 1);
	return HRTIMER_NORESTART;
}

static int periodic_fired;
static unsigned long long last_overruns;
static const ktime_t period = 2 * NSEC_PER_MSEC;
static enum hrtimer_restart periodic(struct hrtimer *t)
{
	T_STORE(&last_overruns, hrtimer_forward_now(t, period));
	T_ADD(&periodic_fired, 1);
	return HRTIMER_RESTART;
}

static int blocked_entered, blocked_release, blocked_done;
static enum hrtimer_restart blocking(struct hrtimer *t)
{
	(void)t;
	T_STORE(&blocked_entered, 1);
	while (!T_LOAD(&blocked_release))
		usleep(100);
	T_STORE(&blocked_done, 1);
	return HRTIMER_NORESTART;
}

static int list_fired;
static void list_fn(struct timer_list *t)
{
	(void)t;
	T_ADD(&list_fired, 1);
}

static void *release_later(void *arg)
{
	(void)arg;
	usleep(20000);
	T_STORE(&blocked_release, 1);
	return NULL;
}

static void wait_for(int *v, int value)
{
	for (int i = 0; i < 4000 && T_LOAD(v) < value; i++)
		usleep(500);
	assert(T_LOAD(v) >= value);
}

int main(void)
{
	struct hrtimer t;
	assert(linuxu_timer_service_init() == 0);

	/* hrtimer_forward(): expiry 100, now 350, interval 100 -> 3 overruns,
	 * new expiry 400 (Linux arithmetic, exact-multiple correction). */
	hrtimer_setup(&t, oneshot, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	hrtimer_set_expires(&t, 100);
	assert(hrtimer_forward(&t, 350, 100) == 3);
	assert(hrtimer_get_expires(&t) == 400);
	assert(hrtimer_forward(&t, 350, 100) == 0);	/* not yet expired */
	hrtimer_set_expires(&t, 100);
	assert(hrtimer_forward(&t, 300, 100) == 3);	/* exact multiple */
	assert(hrtimer_get_expires(&t) == 400);

	/* One-shot relative timer fires once and becomes inactive. */
	hrtimer_setup(&t, oneshot, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	assert(!hrtimer_active(&t));
	ktime_t before = ktime_get();
	hrtimer_start(&t, 3 * NSEC_PER_MSEC, HRTIMER_MODE_REL);
	assert(hrtimer_active(&t) && hrtimer_is_queued(&t));
	wait_for(&oneshot_fired, 1);
	assert(ktime_get() - before >= 3 * NSEC_PER_MSEC);
	usleep(10000);
	assert(T_LOAD(&oneshot_fired) == 1);
	assert(!hrtimer_active(&t));
	assert(hrtimer_cancel(&t) == 0);
	assert(hrtimer_try_to_cancel(&t) == 0);

	/* Cancel of a pending timer: 1, and it never fires. */
	hrtimer_start(&t, 50 * NSEC_PER_MSEC, HRTIMER_MODE_REL);
	assert(hrtimer_try_to_cancel(&t) == 1);
	usleep(80000);
	assert(T_LOAD(&oneshot_fired) == 1);

	/* Periodic timer: restarts itself; hrtimer_cancel() stops it. */
	struct hrtimer p;
	hrtimer_setup(&p, periodic, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&p, period, HRTIMER_MODE_REL);
	wait_for(&periodic_fired, 10);
	assert(T_LOAD(&last_overruns) >= 1);
	assert(hrtimer_cancel(&p) == 1 || !hrtimer_active(&p));
	assert(!hrtimer_active(&p));
	int stopped = T_LOAD(&periodic_fired);
	usleep(20000);
	assert(T_LOAD(&periodic_fired) == stopped);

	/* drm_vblank_cancel_timer idiom on a running periodic timer. */
	hrtimer_start(&p, period, HRTIMER_MODE_REL);
	wait_for(&periodic_fired, stopped + 3);
	while (hrtimer_active(&p))
		hrtimer_try_to_cancel(&p);
	stopped = T_LOAD(&periodic_fired);
	usleep(20000);
	assert(T_LOAD(&periodic_fired) == stopped);

	/* While a callback runs: try_to_cancel() is -1, hrtimer_active() is
	 * true, and hrtimer_cancel() waits for the callback to return. */
	struct hrtimer b;
	pthread_t releaser;
	hrtimer_setup(&b, blocking, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&b, 1 * NSEC_PER_MSEC, HRTIMER_MODE_REL);
	wait_for(&blocked_entered, 1);
	assert(hrtimer_try_to_cancel(&b) == -1);
	assert(hrtimer_active(&b));
	assert(pthread_create(&releaser, NULL, release_later, NULL) == 0);
	assert(hrtimer_cancel(&b) == 0);
	assert(T_LOAD(&blocked_done) == 1);
	assert(!hrtimer_active(&b));
	pthread_join(releaser, NULL);

	/* timer_list timers still run on the shared worker, and a far
	 * jiffies deadline does not delay a near hrtimer. */
	struct timer_list far, near;
	timer_setup(&far, list_fn, 0);
	timer_setup(&near, list_fn, 0);
	mod_timer(&far, jiffies + 10 * HZ);
	mod_timer(&near, jiffies + 1);
	before = ktime_get();
	hrtimer_start(&t, 2 * NSEC_PER_MSEC, HRTIMER_MODE_REL);
	wait_for(&oneshot_fired, 2);
	assert(ktime_get() - before < 500 * NSEC_PER_MSEC);
	wait_for(&list_fired, 1);
	assert(del_timer_sync(&far) == 1);
	assert(T_LOAD(&list_fired) == 1);

	printf("hrtimer one-shot, periodic forward/restart, cancel-while-running "
	       "and timer_list coexistence passed\n");
	return 0;
}
