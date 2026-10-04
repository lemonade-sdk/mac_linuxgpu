#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <rt/park.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) { return linuxu_park_now_ns(); }

/* Two waiters on two queues; each queue is woken by its own producer. */
static struct wait_queue_head queue_a, queue_b;
static int flag_a, flag_b;
static uint64_t woke_a_ns, woke_b_ns;
static void *waiter_a(void *arg)
{
	(void)arg;
	wait_event(queue_a, __atomic_load_n(&flag_a, __ATOMIC_ACQUIRE));
	woke_a_ns = now_ns();
	return NULL;
}
static void *waiter_b(void *arg)
{
	(void)arg;
	wait_event(queue_b, __atomic_load_n(&flag_b, __ATOMIC_ACQUIRE));
	woke_b_ns = now_ns();
	return NULL;
}
static struct task_struct *sleeper_task;
static long sleeper_left;
static uint64_t sleeper_woke_ns;
static void *sleeper(void *arg)
{
	(void)arg;
	__atomic_store_n(&sleeper_task, current, __ATOMIC_RELEASE);
	set_current_state(TASK_INTERRUPTIBLE);
	sleeper_left = schedule_timeout(msecs_to_jiffies(2000));
	sleeper_woke_ns = now_ns();
	return NULL;
}

/* Interrupt-driven waits (rt/park.h): the producer's wake ends the sleep,
 * an idle waiter costs only its backstops, a lost wake is caught. */
static void park_checks(void)
{
	struct linuxu_park_stats before, after;
	pthread_t a, b, s;
	uint64_t t0;

	init_waitqueue_head(&queue_a);
	init_waitqueue_head(&queue_b);
	/* Backstops far longer than the test's latencies: only wakes count. */
	linuxu_park_set_backstop(500000000ULL, 1000000000ULL);
	assert(!pthread_create(&a, NULL, waiter_a, NULL));
	assert(!pthread_create(&b, NULL, waiter_b, NULL));
	usleep(20000);
	t0 = now_ns();
	__atomic_store_n(&flag_a, 1, __ATOMIC_RELEASE);
	wake_up(&queue_a);
	assert(!pthread_join(a, NULL));
	assert(woke_a_ns - t0 < 20000000ULL);	/* the wake, not a backstop */
	usleep(20000);
	assert(!woke_b_ns);			/* the other waiter still sleeps */
	__atomic_store_n(&flag_b, 1, __ATOMIC_RELEASE);
	wake_up(&queue_b);
	assert(!pthread_join(b, NULL));
	printf("park: wake_up woke its waiter in %llu us; the other slept on\n",
	       (unsigned long long)(woke_a_ns - t0) / 1000);

	/* schedule_timeout ends at wake_up_process. */
	assert(!pthread_create(&s, NULL, sleeper, NULL));
	while (!__atomic_load_n(&sleeper_task, __ATOMIC_ACQUIRE))
		usleep(1000);
	usleep(20000);
	t0 = now_ns();
	wake_up_process(sleeper_task);
	assert(!pthread_join(s, NULL));
	assert(sleeper_woke_ns - t0 < 20000000ULL && sleeper_left > msecs_to_jiffies(1000));

	/* An idle wait: 300 ms with the default backstop (2 ms doubling to
	 * 1 s) wakes a handful of times, not every millisecond. */
	linuxu_park_set_backstop(LINUXU_PARK_BACKSTOP_FIRST_NS, LINUXU_PARK_BACKSTOP_MAX_NS);
	linuxu_park_stats(&before);
	t0 = now_ns();
	assert(wait_event_timeout(queue_a, false, msecs_to_jiffies(300)) == 0);
	assert(now_ns() - t0 >= 299000000ULL);
	linuxu_park_stats(&after);
	printf("park: an idle 300 ms wait woke %llu times (polling every 1 ms: 300)\n",
	       after.wakeups - before.wakeups);
	assert(after.wakeups - before.wakeups <= 12);

	/* A completion: complete() wakes it at once. */
	{
		struct completion c;

		init_completion(&c);
		linuxu_park_stats(&before);
		complete(&c);
		assert(wait_for_completion_timeout(&c, msecs_to_jiffies(100)) > 0);
	}
}

static struct wait_queue_head queue;
static DEFINE_SPINLOCK(lock);
static int ready;
static void *producer(void *arg)
{
	(void)arg;
	msleep(15);
	spin_lock(&lock);
	ready = 1;
	spin_unlock(&lock);
	return NULL;
}
int main(void)
{
	assert(wait_event_timeout(queue, true, 0) == 1);
	assert(wait_event_timeout(queue, false, 0) == 0);
	unsigned long start = jiffies;
	assert(wait_event_interruptible_timeout(queue, false, msecs_to_jiffies(30)) == 0);
	assert(jiffies - start >= msecs_to_jiffies(30));
	assert(jiffies - start < msecs_to_jiffies(1000));
	pthread_t thread;
	assert(pthread_create(&thread, NULL, producer, NULL) == 0);
	spin_lock(&lock);
	assert(wait_event_interruptible_lock_irq(queue, ready, lock) == 0);
	assert(ready && spin_is_locked(&lock));
	spin_unlock(&lock);
	assert(pthread_join(thread, NULL) == 0);
	assert(wait_event_interruptible(queue, true) == 0);
	/* Infinite waits must stay positive; finite waits retain their budget
	 * between condition checks instead of expiring after microseconds. */
	assert(MAX_SCHEDULE_TIMEOUT > 0);
	assert(schedule_timeout(MAX_SCHEDULE_TIMEOUT) == MAX_SCHEDULE_TIMEOUT);
	assert(schedule_timeout(0) == 0);
	assert(schedule_timeout(-1) == 0);
	start = jiffies;
	long left = 3;
	while (left) {
		long previous = left;
		left = schedule_timeout(left);
		assert(left >= 0 && left <= previous);
	}
	assert(jiffies - start >= 3 && jiffies - start < 100);
	{
		/* A running task does not sleep in schedule_timeout or
		 * schedule (a poll, or a wake that came first): the whole
		 * timeout is left, as in Linux. */
		struct timespec a, b;

		__set_current_state(TASK_RUNNING);
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (int i = 0; i < 1000; i++) {
			assert(schedule_timeout(msecs_to_jiffies(50)) >= msecs_to_jiffies(50) - 1);
			schedule();
		}
		clock_gettime(CLOCK_MONOTONIC, &b);
		/* 2000 calls: a millisecond sleep in any of them shows. */
		assert((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) < 500000000L);
		/* A sleeping state still sleeps the timeout. */
		start = jiffies;
		__set_current_state(TASK_UNINTERRUPTIBLE);
		assert(schedule_timeout(msecs_to_jiffies(20)) == 0);
		assert(jiffies - start >= msecs_to_jiffies(20));
	}
	{
		/* The producer above set ready without a wake: the backstop
		 * found it and counted the missed wake. */
		struct linuxu_park_stats st;

		linuxu_park_stats(&st);
		assert(st.missed >= 1);
	}
	park_checks();
	puts("wait_event immediate condition, finite timeout, lock release/reacquire: PASS");
}
