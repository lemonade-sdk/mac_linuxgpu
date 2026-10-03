/* Workqueue concurrency contracts upstream amdgpu/amdkfd/TTM rely on:
 * eager system queues, independent worker pools, max_active, ordered FIFO,
 * non-reentrancy, delayed work under load and cancel/execute races.
 * Every wait is bounded; a watchdog turns a deadlock into a failure. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

static atomic_int printk_calls;
int printk(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	return atomic_fetch_add_explicit(&printk_calls, 1, memory_order_seq_cst) + 1;
}

static const char *_Atomic current_test = "startup";
static atomic_int all_done;

static void *watchdog(void *arg)
{
	(void)arg;
	for (int i = 0; i < 1200; ++i) { /* 120 s for the whole binary */
		if (atomic_load(&all_done))
			return NULL;
		usleep(100 * 1000);
	}
	fprintf(stderr, "FAIL watchdog: \"%s\" did not finish (deadlock?)\n",
		atomic_load(&current_test));
	_exit(2);
}

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* Wait up to ms for *value to reach at least expected. */
static int wait_at_least(atomic_int *value, int expected, int ms)
{
	long long end = now_ms() + ms;
	while (atomic_load(value) < expected) {
		if (now_ms() > end)
			return 0;
		usleep(200);
	}
	return 1;
}

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s: %s:%d: %s\n",			\
			atomic_load(&current_test), __FILE__, __LINE__, #cond); \
		_exit(1);						\
	}								\
} while (0)

static void begin(const char *name)
{
	atomic_store(&current_test, name);
	fprintf(stderr, "-- %s\n", name);
}

static void add_one(atomic_int *value)
{
	atomic_fetch_add_explicit(value, 1, memory_order_seq_cst);
}

/* ---- 1. system queues exist before any schedule_work() ---- */
struct counted { struct work_struct work; atomic_int runs; };
static void counted_fn(struct work_struct *work)
{
	add_one(&container_of(work, struct counted, work)->runs);
}

static void test_system_queues_eager(void)
{
	begin("system queues are usable before schedule_work");
	struct workqueue_struct *queues[] = {
		system_dfl_wq, system_wq, system_highpri_wq, system_unbound_wq,
		system_unbound_highpri_wq, system_freezable_wq, system_long_wq,
		system_power_wq, system_switch_wq, system_freezable_highpri_wq,
		system_unbound_dfl_wq, system_percpu_wq,
	};
	static struct counted items[sizeof(queues) / sizeof(queues[0])];
	int warnings = atomic_load(&printk_calls);

	for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i) {
		CHECK(queues[i]);
		INIT_WORK(&items[i].work, counted_fn);
		CHECK(queue_work(queues[i], &items[i].work));
	}
	for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i) {
		flush_work(&items[i].work);
		CHECK(atomic_load(&items[i].runs) == 1);
	}
	for (size_t i = 0; i < sizeof(queues) / sizeof(queues[0]); ++i)
		for (size_t j = i + 1; j < sizeof(queues) / sizeof(queues[0]); ++j)
			if (queues[j] != system_percpu_wq && queues[i] != system_percpu_wq)
				CHECK(queues[i] != queues[j]);
	/* Delayed work on a system queue, still before schedule_work(). */
	static struct delayed_work dw;
	static struct counted *unused;
	(void)unused;
	INIT_DELAYED_WORK(&dw, counted_fn);
	CHECK(queue_delayed_work(system_dfl_wq, &dw, 1));
	flush_delayed_work(&dw);
	CHECK(atomic_load(&printk_calls) == warnings); /* no NULL fallback */
}

/* ---- 2. schedule_work + flush_work from a system work item (kfd) ---- */
static atomic_int inner_ran, outer_done;
static void inner_fn(struct work_struct *work)
{
	(void)work;
	add_one(&inner_ran);
}
static void outer_fn(struct work_struct *work)
{
	struct work_struct inner;
	(void)work;
	INIT_WORK_ONSTACK(&inner, inner_fn);
	schedule_work(&inner);
	flush_work(&inner);
	destroy_work_on_stack(&inner);
	CHECK(atomic_load(&inner_ran) == 1);
	add_one(&outer_done);
}

static void test_nested_flush(void)
{
	begin("schedule_work + flush_work inside a system work item");
	struct work_struct outer;
	INIT_WORK(&outer, outer_fn);
	CHECK(schedule_work(&outer));
	CHECK(wait_at_least(&outer_done, 1, 5000));
	flush_work(&outer);

	/* Same pattern on system_dfl_wq and system_freezable_wq. */
	atomic_store(&inner_ran, 0); atomic_store(&outer_done, 0);
	CHECK(queue_work(system_freezable_wq, &outer));
	CHECK(wait_at_least(&outer_done, 1, 5000));
	flush_work(&outer);
}

/* ---- 3. one system item blocks on a completion another signals ---- */
static struct completion handoff;
static atomic_int waiter_done, signaller_done;
static void waiter_fn(struct work_struct *work)
{
	(void)work;
	wait_for_completion(&handoff);
	add_one(&waiter_done);
}
static void signaller_fn(struct work_struct *work)
{
	(void)work;
	complete(&handoff);
	add_one(&signaller_done);
}

static void test_blocking_pair(void)
{
	begin("blocked system work does not stall other system work");
	struct work_struct waiter, signaller;
	init_completion(&handoff);
	INIT_WORK(&waiter, waiter_fn);
	INIT_WORK(&signaller, signaller_fn);
	CHECK(schedule_work(&waiter));
	usleep(20 * 1000); /* let the waiter occupy a worker */
	CHECK(schedule_work(&signaller));
	CHECK(wait_at_least(&signaller_done, 1, 5000));
	CHECK(wait_at_least(&waiter_done, 1, 5000));
	flush_work(&waiter);
	flush_work(&signaller);
}

/* ---- 4. ordered queues: strict FIFO, one at a time ---- */
#define FIFO_ITEMS 64
struct fifo_item { struct work_struct work; int index; };
static pthread_mutex_t fifo_lock = PTHREAD_MUTEX_INITIALIZER;
static int fifo_order[FIFO_ITEMS], fifo_count;
static atomic_int fifo_inside, fifo_max_inside;
static void fifo_fn(struct work_struct *work)
{
	struct fifo_item *item = container_of(work, struct fifo_item, work);
	int inside = atomic_fetch_add_explicit(&fifo_inside, 1, memory_order_seq_cst) + 1;
	int seen = atomic_load(&fifo_max_inside);
	while (inside > seen &&
	       !atomic_compare_exchange_weak(&fifo_max_inside, &seen, inside))
		;
	if (item->index == 0)
		usleep(20 * 1000); /* let the rest pile up behind the head */
	else
		usleep(200);
	pthread_mutex_lock(&fifo_lock);
	fifo_order[fifo_count++] = item->index;
	pthread_mutex_unlock(&fifo_lock);
	atomic_fetch_sub_explicit(&fifo_inside, 1, memory_order_seq_cst);
}

static void check_fifo(struct workqueue_struct *wq)
{
	static struct fifo_item items[FIFO_ITEMS];
	fifo_count = 0;
	atomic_store(&fifo_max_inside, 0);
	CHECK(wq);
	CHECK(workqueue_is_single_threaded(wq));
	for (int i = 0; i < FIFO_ITEMS; ++i) {
		INIT_WORK(&items[i].work, fifo_fn);
		items[i].index = i;
		CHECK(queue_work(wq, &items[i].work));
	}
	flush_workqueue(wq);
	CHECK(fifo_count == FIFO_ITEMS);
	for (int i = 0; i < FIFO_ITEMS; ++i)
		CHECK(fifo_order[i] == i);
	CHECK(atomic_load(&fifo_max_inside) == 1);
	destroy_workqueue(wq);
}

static void test_ordered_fifo(void)
{
	begin("ordered queues run FIFO, one item at a time");
	check_fifo(alloc_ordered_workqueue("fifo-ordered", 0));
	check_fifo(create_singlethread_workqueue("fifo-single"));
	check_fifo(alloc_workqueue("fifo-max1", WQ_UNBOUND, 1));
}

/* ---- 5. max_active is honoured; concurrency is real ---- */
static atomic_int pool_running, pool_release;
static void pool_fn(struct work_struct *work)
{
	(void)work;
	add_one(&pool_running);
	while (!atomic_load(&pool_release))
		usleep(500);
	atomic_fetch_sub_explicit(&pool_running, 1, memory_order_seq_cst);
}

static void check_pool(struct workqueue_struct *wq, int limit, int items)
{
	struct work_struct *works = calloc((size_t)items, sizeof(*works));
	CHECK(wq && works);
	atomic_store(&pool_running, 0);
	atomic_store(&pool_release, 0);
	for (int i = 0; i < items; ++i) {
		INIT_WORK(&works[i], pool_fn);
		CHECK(queue_work(wq, &works[i]));
	}
	CHECK(wait_at_least(&pool_running, limit, 5000));
	usleep(100 * 1000);
	CHECK(atomic_load(&pool_running) == limit); /* never above max_active */
	atomic_store(&pool_release, 1);
	flush_workqueue(wq);
	CHECK(atomic_load(&pool_running) == 0);
	destroy_workqueue(wq);
	free(works);
}

static void test_max_active(void)
{
	begin("non-ordered queues run up to max_active items at once");
	check_pool(alloc_workqueue("pool-4", WQ_UNBOUND, 4), 4, 8);
	/* TTM's delete queue: max_active 16 is not serialized. */
	check_pool(alloc_workqueue("ttm-like", WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_UNBOUND, 16),
		   16, 20);
	/* 0 selects the default, capped at the shim's pool size of 16. */
	check_pool(alloc_workqueue("pool-dfl", 0, 0), 16, 24);
}

/* ---- 6. a work item never runs concurrently with itself ---- */
static struct workqueue_struct *reent_a, *reent_b;
static struct work_struct reent_work;
static atomic_int reent_inside, reent_overlap, reent_runs;
static void reent_fn(struct work_struct *work)
{
	(void)work;
	if (atomic_fetch_add_explicit(&reent_inside, 1, memory_order_seq_cst))
		add_one(&reent_overlap);
	usleep(300);
	add_one(&reent_runs);
	atomic_fetch_sub_explicit(&reent_inside, 1, memory_order_seq_cst);
}
static atomic_int reent_stop;
static void *reent_producer(void *arg)
{
	struct workqueue_struct *wq = arg;
	while (!atomic_load(&reent_stop)) {
		queue_work(wq, &reent_work);
		usleep(50);
	}
	return NULL;
}

static void test_non_reentrant(void)
{
	begin("a single work item is never run concurrently");
	pthread_t producers[4];
	reent_a = alloc_workqueue("reent-a", 0, 16);
	reent_b = alloc_workqueue("reent-b", WQ_UNBOUND, 16);
	CHECK(reent_a && reent_b);
	INIT_WORK(&reent_work, reent_fn);
	for (int i = 0; i < 4; ++i)
		CHECK(!pthread_create(&producers[i], NULL, reent_producer,
				      i & 1 ? reent_b : reent_a));
	usleep(300 * 1000);
	atomic_store(&reent_stop, 1);
	for (int i = 0; i < 4; ++i)
		pthread_join(producers[i], NULL);
	flush_work(&reent_work);
	CHECK(cancel_work_sync(&reent_work) == false);
	CHECK(atomic_load(&reent_runs) > 10);
	CHECK(atomic_load(&reent_overlap) == 0);
	destroy_workqueue(reent_a);
	destroy_workqueue(reent_b);
}

/* ---- 7. delayed work fires on time while its queue is busy ---- */
static atomic_int churn_stop, churn_runs;
static struct workqueue_struct *churn_wq;
static struct work_struct churn_work;
static void churn_fn(struct work_struct *work)
{
	add_one(&churn_runs);
	usleep(2000);
	if (!atomic_load(&churn_stop))
		queue_work(churn_wq, work); /* the queue is never empty */
}
static atomic_llong delayed_at;
static void stamp_fn(struct work_struct *work)
{
	(void)work;
	atomic_store(&delayed_at, now_ms());
}

static void check_delayed_under_load(struct workqueue_struct *wq, bool blocker)
{
	struct delayed_work dw;
	struct work_struct block;
	churn_wq = wq;
	CHECK(wq);
	atomic_store(&churn_stop, 0); atomic_store(&churn_runs, 0);
	atomic_store(&delayed_at, 0);
	atomic_store(&pool_release, 0); atomic_store(&pool_running, 0);
	INIT_WORK(&churn_work, churn_fn);
	CHECK(queue_work(wq, &churn_work));
	if (blocker) {
		INIT_WORK(&block, pool_fn); /* occupies one worker until released */
		CHECK(queue_work(wq, &block));
	}
	CHECK(wait_at_least(&churn_runs, 5, 5000));
	INIT_DELAYED_WORK(&dw, stamp_fn);
	long long queued_at = now_ms();
	CHECK(queue_delayed_work(wq, &dw, msecs_to_jiffies(50)));
	long long deadline = now_ms() + 3000;
	while (!atomic_load(&delayed_at) && now_ms() < deadline)
		usleep(1000);
	long long late = atomic_load(&delayed_at) - queued_at;
	CHECK(atomic_load(&delayed_at) != 0); /* not starved by the churn */
	CHECK(late >= 30);
	CHECK(late < 1000);
	atomic_store(&churn_stop, 1);
	atomic_store(&pool_release, 1);
	cancel_work_sync(&churn_work);
	flush_workqueue(wq);
	destroy_workqueue(wq);
}

static void test_delayed_under_load(void)
{
	begin("delayed work runs on time while the queue is busy");
	check_delayed_under_load(alloc_workqueue("busy-pool", 0, 4), true);
	check_delayed_under_load(alloc_ordered_workqueue("busy-ordered", 0), false);
}

/* ---- 8. cancel_delayed_work_sync racing with execution ---- */
static struct workqueue_struct *race_wq;
static struct delayed_work race_dw;
static atomic_int race_inside, race_runs, race_requeue;
static void race_fn(struct work_struct *work)
{
	add_one(&race_inside);
	usleep((unsigned int)(rand() % 400));
	add_one(&race_runs);
	if (atomic_load(&race_requeue))
		queue_delayed_work(race_wq, to_delayed_work(work), 0);
	atomic_fetch_sub_explicit(&race_inside, 1, memory_order_seq_cst);
}

static void test_cancel_race(void)
{
	begin("cancel_delayed_work_sync racing with execution");
	race_wq = alloc_workqueue("cancel-race", 0, 8);
	CHECK(race_wq);
	INIT_DELAYED_WORK(&race_dw, race_fn);
	srand(1234);
	for (int i = 0; i < 300; ++i) {
		atomic_store(&race_requeue, i & 1);
		queue_delayed_work(race_wq, &race_dw, (unsigned long)(i % 3));
		usleep((unsigned int)(rand() % 600));
		cancel_delayed_work_sync(&race_dw);
		CHECK(!work_pending(&race_dw.work));
		CHECK(atomic_load(&race_inside) == 0);
		CHECK(!(work_busy(&race_dw.work) & WORK_BUSY_RUNNING));
		int runs = atomic_load(&race_runs);
		if (i % 50 == 0) {
			usleep(30 * 1000); /* a cancelled chain stays cancelled */
			CHECK(atomic_load(&race_runs) == runs);
		}
	}
	/* mod_delayed_work against a running instance, then cancel. */
	atomic_store(&race_requeue, 0);
	for (int i = 0; i < 100; ++i) {
		mod_delayed_work(race_wq, &race_dw, 0);
		usleep((unsigned int)(rand() % 300));
		mod_delayed_work(race_wq, &race_dw, (unsigned long)(i % 2));
		cancel_delayed_work_sync(&race_dw);
		CHECK(!work_pending(&race_dw.work));
		CHECK(atomic_load(&race_inside) == 0);
	}
	destroy_workqueue(race_wq);
}

/* ---- 9. flush_work is per item; flush_workqueue is a snapshot ---- */
static atomic_int hold_release, hold_started;
static void hold_fn(struct work_struct *work)
{
	(void)work;
	add_one(&hold_started);
	while (!atomic_load(&hold_release))
		usleep(500);
}
static atomic_int qflush_done;
static void *qflush_thread(void *wq)
{
	flush_workqueue(wq);
	add_one(&qflush_done);
	return NULL;
}

static void test_flush_scope(void)
{
	begin("flush_work waits only for its item; flush_workqueue for prior work");
	struct workqueue_struct *wq = alloc_workqueue("flush-scope", 0, 4);
	struct work_struct hold, quick;
	struct counted after;
	pthread_t flusher;
	CHECK(wq);
	atomic_store(&hold_release, 0); atomic_store(&hold_started, 0);
	INIT_WORK(&hold, hold_fn);
	INIT_WORK(&quick, inner_fn);
	INIT_WORK(&after.work, counted_fn);
	atomic_store(&after.runs, 0);
	atomic_store(&inner_ran, 0);
	CHECK(queue_work(wq, &hold));
	CHECK(wait_at_least(&hold_started, 1, 5000));
	CHECK(queue_work(wq, &quick));
	CHECK(flush_work(&quick)); /* returns although hold still runs */
	CHECK(atomic_load(&inner_ran) == 1);
	CHECK(!pthread_create(&flusher, NULL, qflush_thread, wq));
	usleep(50 * 1000);
	CHECK(queue_work(wq, &after.work)); /* queued after the flush began */
	flush_work(&after.work);
	CHECK(atomic_load(&after.runs) == 1);
	CHECK(atomic_load(&qflush_done) == 0); /* still waiting for hold */
	atomic_store(&hold_release, 1);
	pthread_join(flusher, NULL);
	CHECK(atomic_load(&qflush_done) == 1);
	destroy_workqueue(wq);
}

int main(void)
{
	pthread_t dog;
	CHECK(!pthread_create(&dog, NULL, watchdog, NULL));
	/* The driver bootstrap does this before any module init. */
	CHECK(linuxu_workqueue_init() == 0);
	test_system_queues_eager();
	test_nested_flush();
	test_blocking_pair();
	test_ordered_fifo();
	test_max_active();
	test_non_reentrant();
	test_delayed_under_load();
	test_cancel_race();
	test_flush_scope();
	atomic_store(&all_done, 1);
	pthread_join(dog, NULL);
	puts("workqueue concurrency: eager system queues, nested flush, blocking "
	     "pairs, ordered FIFO, max_active, non-reentrancy, delayed under load "
	     "and cancel races passed");
	return 0;
}
