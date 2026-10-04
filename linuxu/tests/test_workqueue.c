/* test_workqueue.c — workqueue (REAL): schedule_work runs the
 * worker; a delayed work respects its jiffy delay (>= delay-2 jiffies);
 * cancel_delayed_work_sync prevents a queued-but-not-fired work.
 * HZ=100 (shared jiffies contract: 10ms per jiffy). */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

static int ran = 0;
static int delayed_ms = -1;
static int cancel_ran = 0;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t start_ms = 0;

static void work_fn(struct work_struct *work)
{
	(void)work;
	ran = 1;
}

static void delayed_fn(struct work_struct *work)
{
	(void)work;
	delayed_ms = (int)(now_ms() - start_ms);
}

static void cancel_fn(struct work_struct *work)
{
	(void)work;
	cancel_ran = 1;
}

int main(void)
{
	struct work_struct work;
	struct delayed_work dwork;
	struct delayed_work cwork;

	/* 1. immediate work runs */
	INIT_WORK(&work, work_fn);
	schedule_work(&work);
	usleep(50 * 1000);
	EXPECT(ran == 1);

	/* 2. delayed work: deadline is 50ms at the shared HZ. */
	INIT_DELAYED_WORK(&dwork, delayed_fn);
	start_ms = now_ms();
	schedule_delayed_work(&dwork, msecs_to_jiffies(50));
	usleep(200 * 1000);
	/* Absolute jiffies are quantized to a jiffy; a deadline may land
	 * just under the requested wall-clock duration. */
	EXPECT(delayed_ms >= 40);
	EXPECT(delayed_ms < 150);

	/* 3. cancel_delayed_work_sync: queue a long-delay work, cancel
	 * before it fires; it must never run */
	INIT_DELAYED_WORK(&cwork, cancel_fn);
	schedule_delayed_work(&cwork, msecs_to_jiffies(4000));
	EXPECT(cancel_delayed_work_sync(&cwork) == true);
	usleep(80 * 1000);
	EXPECT(cancel_ran == 0);

	fprintf(stderr, "PASS test_workqueue\n");
	return 0;
}
