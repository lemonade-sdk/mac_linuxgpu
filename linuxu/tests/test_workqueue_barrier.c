/* test_workqueue_barrier.c — W4: the workqueue drain barriers are REAL.
 *
 * 1. flush_work is a true barrier: a queued work (flag + completion)
 *    has fully finished before flush_work returns.
 * 2. cancel_work_sync on a RUNNING work blocks until the work function
 *    returns (measured from a second thread) — it does not just drop.
 * 3. No UAF: after a flush, the same work is re-queued and flushed
 *    again; the workqueue is destroyed after a final drain; kmemcheck
 *    is clean at the end.
 */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <linux/workqueue.h>
#include <linux/completion.h>

extern int kmemcheck_verify_all(void);

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- 1. flush_work drains a queued work ---- */
static int flush_ran = 0;
static struct completion flush_done;

static void flush_work_fn(struct work_struct *work)
{
	(void)work;
	flush_ran = 1;
	complete(&flush_done);
}

static int test_flush_work_drains(void)
{
	struct workqueue_struct *wq = alloc_workqueue("t4-flush", 0, 0);
	struct work_struct work;

	INIT_WORK(&work, flush_work_fn);
	EXPECT(queue_work(wq, &work) == true);
	flush_work(&work);
	/* both must hold BEFORE flush_work returned: the barrier is real */
	EXPECT(flush_ran == 1);
	EXPECT(completion_done(&flush_done) == 1);
	/* flush of an already-run work must be a no-op that returns */
	flush_work(&work);
	destroy_workqueue(wq);
	return 0;
}

/* ---- 2. cancel_work_sync on a running work drains it ---- */
#define LONG_WORK_MS 300
static _Atomic(int) long_started;
static _Atomic(int) long_finished;
static _Atomic(long long) long_finished_ms; /* real finish time, in the work fn */

static void long_work_fn(struct work_struct *work)
{
	(void)work;
	long_started = 1;
	usleep(LONG_WORK_MS * 1000);
	long_finished_ms = now_ms(); /* record BEFORE setting the flag */
	long_finished = 1;
}

static struct {
	pthread_t tid;
	int64_t start_ms;   /* when the work fn started */
	int64_t ret_ms;     /* when cancel_work_sync returned */
	int ret_val;        /* cancel_work_sync's return */
} cancel_probe;

static void *cancel_thread(void *arg)
{
	struct work_struct *work = arg;
	int64_t t;

	/* wait until the work is actually running on the worker */
	while (!long_started)
		usleep(200);
	/* let the work run a bit so the cancel definitely hits mid-run */
	usleep(50 * 1000);
	cancel_probe.start_ms = now_ms();
	cancel_probe.ret_val = (int)cancel_work_sync(work);
	cancel_probe.ret_ms = now_ms();
	return NULL;
}

static int test_cancel_work_sync_drains(void)
{
	struct workqueue_struct *wq = alloc_workqueue("t4-cancel", 0, 0);
	struct work_struct work;
	int64_t finished_ms;
	int64_t block_ms;
	int rc;

	INIT_WORK(&work, long_work_fn);
	long_started = 0;
	long_finished = 0;
	rc = pthread_create(&cancel_probe.tid, NULL, cancel_thread, &work);
	if (rc)
		return 1;
	EXPECT(queue_work(wq, &work) == true);

	pthread_join(cancel_probe.tid, NULL);
	/* the work's REAL finish time (recorded inside long_work_fn) must be
	 * <= when cancel_work_sync returned (the barrier held until the func
	 * returned). Reading now_ms() here would be after pthread_join and
	 * thus always later than ret_ms — that was the original bug. */
	finished_ms = (int64_t)long_finished_ms;

	/* Linux returns true only for a cancelled pending instance. This work
	 * is already running, so the result is false after waiting for it. */
	EXPECT(cancel_probe.ret_val == 0);
	/* it blocked for a meaningful share of the work's runtime —
	 * i.e. it waited for the in-flight func, it did not drop it */
	block_ms = cancel_probe.ret_ms - cancel_probe.start_ms;
	fprintf(stderr, "    (cancel_work_sync blocked %lld ms of a %d ms work)\n",
		(long long)block_ms, LONG_WORK_MS);
	EXPECT(block_ms >= 50);
	/* the work ran to completion: finished before the cancel call
	 * returned (the barrier held until the func returned) */
	EXPECT(long_finished == 1);
	EXPECT(finished_ms <= cancel_probe.ret_ms);
	destroy_workqueue(wq);
	return 0;
}

/* ---- 3. no UAF: re-queue + re-flush the same work; clean destroy ---- */
static int reuse_count = 0;
static struct completion reuse_done;

static void reuse_work_fn(struct work_struct *work)
{
	(void)work;
	reuse_count++;
	complete(&reuse_done);
}

static int test_requeue_no_uaf(void)
{
	struct workqueue_struct *wq = alloc_workqueue("t4-reuse", 0, 0);
	struct work_struct work;
	int i;

	INIT_WORK(&work, reuse_work_fn);
	for (i = 0; i < 5; i++) {
		/* fresh completion each round (init resets the counter) */
		init_completion(&reuse_done);
		EXPECT(queue_work(wq, &work) == true);
		flush_work(&work);
		EXPECT(reuse_count == i + 1);
		EXPECT(completion_done(&reuse_done) == 1);
	}
	/* destroy must drain (it does now) — safe to free the work
	 * struct right after */
	destroy_workqueue(wq);
	return 0;
}

/* ---- flush_workqueue drains everything queued ---- */
static int batch_count = 0;

static void batch_work_fn(struct work_struct *work)
{
	(void)work;
	batch_count++;
}

static int test_flush_workqueue_drains_all(void)
{
	struct workqueue_struct *wq = alloc_workqueue("t4-batch", 0, 1);
	struct work_struct w[8];
	int i;

	for (i = 0; i < 8; i++)
		INIT_WORK(&w[i], batch_work_fn);
	for (i = 0; i < 8; i++)
		EXPECT(queue_work(wq, &w[i]) == true);
	flush_workqueue(wq);
	/* every queued item must have run before the barrier returned */
	EXPECT(batch_count == 8);
	destroy_workqueue(wq);
	return 0;
}

/* Requeueing a running work item must schedule a second worker pass,
 * never recurse into the callback under the queue lock. */
static struct workqueue_struct *requeue_wq;
static _Atomic(int) requeue_count;
static _Atomic(int) requeue_inside;
static _Atomic(int) requeue_recursive;
static _Atomic(int) requeue_accepted;
static _Atomic(int) blocker_started;
static _Atomic(int) duplicate_count;

static void blocker_fn(struct work_struct *work)
{
	(void)work;
	blocker_started = 1;
	usleep(50 * 1000);
}

static void duplicate_fn(struct work_struct *work)
{
	(void)work;
	duplicate_count++;
}

static void requeue_work_fn(struct work_struct *work)
{
	if (requeue_inside++)
		requeue_recursive = 1;
	if (++requeue_count == 1)
		requeue_accepted = queue_work(requeue_wq, work);
	requeue_inside--;
}

static int test_duplicate_and_requeue(void)
{
	struct work_struct work;
	struct work_struct duplicate;
	struct work_struct blocker;

	requeue_wq = create_singlethread_workqueue("t4-requeue");
	INIT_WORK(&work, requeue_work_fn);
	requeue_count = requeue_inside = requeue_recursive = 0;
	requeue_accepted = 0;
	EXPECT(queue_work(requeue_wq, &work));
	/* Drain includes the callback's chained submission. A flush is only a
	 * snapshot of work queued before the flush began. */
	drain_workqueue(requeue_wq);
	EXPECT(requeue_count == 2);
	EXPECT(requeue_accepted == 1);
	EXPECT(requeue_recursive == 0);
	EXPECT(!work_busy(&work));
	INIT_WORK(&blocker, blocker_fn);
	INIT_WORK(&duplicate, duplicate_fn);
	blocker_started = duplicate_count = 0;
	EXPECT(queue_work(requeue_wq, &blocker));
	while (!blocker_started)
		usleep(100);
	EXPECT(queue_work(requeue_wq, &duplicate));
	EXPECT(!queue_work(requeue_wq, &duplicate));
	flush_workqueue(requeue_wq);
	EXPECT(duplicate_count == 1);
	destroy_workqueue(requeue_wq);
	return 0;
}

int main(void)
{
	int rc;

	rc = test_flush_work_drains();
	if (rc)
		return rc;
	rc = test_cancel_work_sync_drains();
	if (rc)
		return rc;
	rc = test_requeue_no_uaf();
	if (rc)
		return rc;
	rc = test_flush_workqueue_drains_all();
	if (rc)
		return rc;
	rc = test_duplicate_and_requeue();
	if (rc)
		return rc;

	/* kmemcheck: no corrupted/kleaked kmem state after all the
	 * queue/flush/cancel/destroy cycles */
	EXPECT(kmemcheck_verify_all() == 0);
	fprintf(stderr, "PASS test_workqueue_barrier\n");
	return 0;
}
