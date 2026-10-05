/* Calls bounded in time (rt/bounded.h, linuxu/src/amdgpu-rt/bounded.c) on
 * the real wait pool: work that finishes in time hands its argument back;
 * work that overruns returns -ETIMEDOUT at the bound and frees its argument
 * itself when it ends. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <rt/bounded.h>

#define LINUX_ETIMEDOUT 110

struct work {
	unsigned int sleep_ms;
	int ran;
	int *freed;
};

static void work_main(void *arg)
{
	struct work *w = arg;

	usleep(w->sleep_ms * 1000);
	w->ran = 1;
}

static void work_release(void *arg)
{
	struct work *w = arg;

	__atomic_store_n(w->freed, 1, __ATOMIC_RELEASE);
	free(w);
}

static double ms_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void)
{
	int freed = 0;
	struct work *w = calloc(1, sizeof(*w));
	double start;

	/* In time: the caller owns the argument, release is not called. */
	w->sleep_ms = 5;
	w->freed = &freed;
	assert(rt_bounded_run(work_main, w, work_release, 500) == 0);
	assert(w->ran && !freed && rt_bounded_overrunning() == 0);
	free(w);

	/* Overrun: -ETIMEDOUT at the bound; the work frees its argument. */
	w = calloc(1, sizeof(*w));
	w->sleep_ms = 300;
	w->freed = &freed;
	start = ms_now();
	assert(rt_bounded_run(work_main, w, work_release, 50) == -LINUX_ETIMEDOUT);
	assert(ms_now() - start >= 45 && ms_now() - start < 250);
	assert(rt_bounded_overrunning() == 1 && !__atomic_load_n(&freed, __ATOMIC_ACQUIRE));
	for (int i = 0; i < 200 && !__atomic_load_n(&freed, __ATOMIC_ACQUIRE); ++i)
		usleep(5000);
	assert(__atomic_load_n(&freed, __ATOMIC_ACQUIRE) && rt_bounded_overrunning() == 0);
	printf("PASS bounded calls: in-time work returns its argument, overrunning work times out "
	       "at the bound and frees its argument when it ends\n");
	return 0;
}
