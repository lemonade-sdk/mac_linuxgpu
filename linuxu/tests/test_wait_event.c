#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <linux/wait.h>
#include <linux/sched.h>

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
	puts("wait_event immediate condition, finite timeout, lock release/reacquire: PASS");
}
