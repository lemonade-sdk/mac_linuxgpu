#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
static int creates, fail_create = 1;
static int timer_test_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*entry)(void *), void *argument)
{
	creates++;
	if (fail_create) return EAGAIN;
	return pthread_create(thread, attr, entry, argument);
}
#define pthread_create timer_test_create
#include "../src/timer.c"
#undef pthread_create
static atomic_int entered, permitted, callbacks;
static void fire(struct timer_list *timer)
{
	(void)timer;
	atomic_fetch_add_explicit(&callbacks, 1, memory_order_seq_cst);
}
static void blocked(struct timer_list *timer)
{
	atomic_store(&entered, 1);
	while (!atomic_load(&permitted)) usleep(100);
	mod_timer(timer, jiffies + 1);
	atomic_fetch_add_explicit(&callbacks, 1, memory_order_seq_cst);
}
static void *shutdown_timer(void *timer)
{ timer_shutdown_sync(timer); return NULL; }
int main(void)
{
	alarm(10);
	assert(linuxu_timer_service_init() == -EAGAIN && creates == 1);
	fail_create = 0;
	assert(!linuxu_timer_service_init() && creates == 2);
	assert(!linuxu_timer_service_init() && creates == 2);
	DEFINE_TIMER(timer, fire);
	unsigned long first = jiffies + HZ * 60;
	assert(timer_reduce(&timer, first) == 0);
	assert(timer_reduce(&timer, first + HZ) == 1 && timer.expires == first);
	assert(timer_reduce(&timer, first - HZ) == 1 && timer.expires == first - HZ);
	assert(timer_delete_sync_try(&timer) == 1);
	assert(timer_delete_sync_try(&timer) == 0);
	timer_setup(&timer, blocked, 0);
	mod_timer(&timer, jiffies + 1);
	while (!atomic_load(&entered)) usleep(100);
	assert(timer_delete_sync_try(&timer) == -1);
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, shutdown_timer, &timer));
	for (;;) {
		pthread_mutex_lock(&timer_lock);
		bool shutting_down = timer.shutdown;
		pthread_mutex_unlock(&timer_lock);
		if (shutting_down) break;
		usleep(100);
	}
	atomic_store(&permitted, 1);
	assert(!pthread_join(thread, NULL));
	assert(!timer_pending(&timer));
	mod_timer(&timer, jiffies + 1); add_timer(&timer); timer_reduce(&timer, jiffies + 1);
	assert(!timer_pending(&timer));
	timer_setup(&timer, fire, 0);
	mod_timer(&timer, jiffies + 1);
	while (atomic_load(&callbacks) != 2) usleep(100);
	timer_shutdown_sync(&timer);
	assert(creates == 2);
	puts("timer preflight start failure, retry, reduce, running deletion and permanent shutdown passed");
}
