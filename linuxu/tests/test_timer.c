/* test_timer.c — timer_list (REAL): mod_timer(100ms) fires;
 * del_timer_sync before expiry prevents the callback. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include <linux/timer.h>
#include <linux/jiffies.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

static int fired = 0;
static int fired2 = 0;
static long long fired_ms;
static atomic_int slow_started;
static atomic_int slow_fired;
static struct timer_list slow_timer;

static void slow_fn(struct timer_list *t)
{
	slow_started = 1;
	slow_fired++;
	usleep(100 * 1000);
	/* A synchronous delete in progress prevents this self-rearm. */
	mod_timer(t, jiffies + msecs_to_jiffies(10));
}

static void *delete_slow(void *unused)
{
	(void)unused;
	while (!slow_started)
		usleep(100);
	del_timer_sync(&slow_timer);
	return NULL;
}

static void timer_fn(struct timer_list *t)
{
	(void)t;
	struct timespec ts;
	clock_gettime(CLOCK_UPTIME_RAW, &ts);
	fired_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	fired = 1;
}

static void timer_fn2(struct timer_list *t)
{
	(void)t;
	fired2 = 1;
}

int main(void)
{
	struct timer_list t1, t2;
	struct timespec start;
	long long start_ms;

	/* 1. mod_timer takes an absolute jiffies deadline. */
	setup_timer(&t1, timer_fn, 0);
	clock_gettime(CLOCK_UPTIME_RAW, &start);
	start_ms = (long long)start.tv_sec * 1000 + start.tv_nsec / 1000000;
	mod_timer(&t1, jiffies + msecs_to_jiffies(100));
	usleep(300 * 1000);
	EXPECT(fired == 1);
	EXPECT(fired_ms - start_ms >= 90);

	/* 2. del_timer_sync before expiry: never fires */
	setup_timer(&t2, timer_fn2, 0);
	mod_timer(&t2, jiffies + msecs_to_jiffies(4000));
	del_timer_sync(&t2);
	usleep(80 * 1000);
	EXPECT(fired2 == 0);

	/* A callback is still using the timer while it runs.  Synchronous
	 * deletion must wait for it and suppress a self-rearm race. */
	{
		pthread_t deleter;
		setup_timer(&slow_timer, slow_fn, 0);
		EXPECT(mod_timer(&slow_timer, jiffies + 1) == 0);
		EXPECT(pthread_create(&deleter, NULL, delete_slow, NULL) == 0);
		pthread_join(deleter, NULL);
		EXPECT(!timer_pending(&slow_timer));
		usleep(50 * 1000);
		EXPECT(slow_fired == 1);
	}

	fprintf(stderr, "PASS test_timer\n");
	return 0;
}
