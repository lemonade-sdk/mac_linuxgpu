/* Calls bounded in time (rt/bounded.h). */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <linux/errno.h>	/* Linux values, as every rt caller compares */
#include <linux/ktime.h>

#include <rt/bounded.h>
#include <rt/wait_pool.h>

/* DriverKit's libsystem_pthread exports only the relative timed wait. */
extern int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex,
					      const struct timespec *relative);

struct bounded {
	pthread_mutex_t lock;
	pthread_cond_t done_cv;
	void (*fn)(void *arg);
	void *arg;
	void (*release)(void *arg);
	bool done;
	bool abandoned;	/* the caller stopped waiting: the thread frees */
};

static unsigned int overrunning;

static void bounded_free(struct bounded *b)
{
	pthread_cond_destroy(&b->done_cv);
	pthread_mutex_destroy(&b->lock);
	free(b);
}

static void bounded_main(void *p)
{
	struct bounded *b = p;
	bool abandoned;

	b->fn(b->arg);
	pthread_mutex_lock(&b->lock);
	b->done = true;
	abandoned = b->abandoned;
	pthread_cond_signal(&b->done_cv);
	pthread_mutex_unlock(&b->lock);
	if (abandoned) {
		if (b->release)
			b->release(b->arg);
		__atomic_sub_fetch(&overrunning, 1, __ATOMIC_ACQ_REL);
		bounded_free(b);
	}
}

int rt_bounded_run(void (*fn)(void *arg), void *arg, void (*release)(void *arg),
		   unsigned int ms)
{
	struct bounded *b;
	uint64_t deadline;
	int r;

	if (!fn)
		return -EINVAL;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	pthread_mutex_init(&b->lock, NULL);
	pthread_cond_init(&b->done_cv, NULL);
	b->fn = fn;
	b->arg = arg;
	b->release = release;
	r = rt_wait_pool_run(bounded_main, b);
	if (r) {
		bounded_free(b);
		return r;
	}
	deadline = ktime_get_ns() + (uint64_t)ms * 1000000ull;
	pthread_mutex_lock(&b->lock);
	while (!b->done) {
		uint64_t now = ktime_get_ns();
		struct timespec rel;

		if (now >= deadline) {
			b->abandoned = true;
			__atomic_add_fetch(&overrunning, 1, __ATOMIC_ACQ_REL);
			pthread_mutex_unlock(&b->lock);
			return -ETIMEDOUT;
		}
		rel.tv_sec = (time_t)((deadline - now) / 1000000000ull);
		rel.tv_nsec = (long)((deadline - now) % 1000000000ull);
		pthread_cond_timedwait_relative_np(&b->done_cv, &b->lock, &rel);
	}
	pthread_mutex_unlock(&b->lock);
	bounded_free(b);
	return 0;
}

unsigned int rt_bounded_overrunning(void)
{
	return __atomic_load_n(&overrunning, __ATOMIC_ACQUIRE);
}
