/* Threads that sleep for clients (rt/wait_pool.h). */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>	/* Linux values, as every rt caller compares */

#include <rt/wait_pool.h>

struct pool_thread {
	pthread_t thread;
	pthread_cond_t wake;
	void (*fn)(void *arg);	/* the job; NULL while idle */
	void *arg;
	struct pool_thread *next_idle;
};

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pool_thread *idle;
static struct rt_wait_pool_stats pool_stats;

static void *pool_main(void *p)
{
	struct pool_thread *t = p;

	pthread_mutex_lock(&pool_lock);
	for (;;) {
		void (*fn)(void *) = t->fn;
		void *arg = t->arg;

		if (!fn) {
			/* Idle: asleep until handed a job, never on a timer. */
			pthread_cond_wait(&t->wake, &pool_lock);
			continue;
		}
		pthread_mutex_unlock(&pool_lock);
		fn(arg);
		pthread_mutex_lock(&pool_lock);
		t->fn = NULL;
		t->arg = NULL;
		pool_stats.busy--;
		t->next_idle = idle;
		idle = t;
	}
	return NULL;
}

int rt_wait_pool_run(void (*fn)(void *arg), void *arg)
{
	struct pool_thread *t;
	int r = 0;

	if (!fn)
		return -EINVAL;
	pthread_mutex_lock(&pool_lock);
	t = idle;
	if (t) {
		idle = t->next_idle;
		t->fn = fn;
		t->arg = arg;
		pool_stats.busy++;
		pool_stats.runs++;
		pthread_cond_signal(&t->wake);
	} else if (pool_stats.threads >= RT_WAIT_POOL_THREADS) {
		pool_stats.refused++;
		r = -EAGAIN;
	} else {
		t = calloc(1, sizeof(*t));
		if (!t) {
			r = -ENOMEM;
		} else {
			pthread_cond_init(&t->wake, NULL);
			t->fn = fn;
			t->arg = arg;
			if (pthread_create(&t->thread, NULL, pool_main, t)) {
				pthread_cond_destroy(&t->wake);
				free(t);
				r = -ENOMEM;
			} else {
				/* Pool threads never exit (DriverKit has no
				 * pthread_detach; nothing joins them). */
				pool_stats.threads++;
				pool_stats.busy++;
				pool_stats.runs++;
			}
		}
	}
	pthread_mutex_unlock(&pool_lock);
	return r;
}

void rt_wait_pool_stats(struct rt_wait_pool_stats *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&pool_lock);
	*out = pool_stats;
	pthread_mutex_unlock(&pool_lock);
}
