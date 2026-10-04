/* Host test for embedded mutex/completion state and jiffy deadlines.
 * Build: clang -w -std=gnu11 -D__KERNEL__ -ffunction-sections
 *   -fdata-sections -Ilinuxu/headers linuxu/tests/test_mutex_completion.c
 *   linuxu/src/sync.c -Wl,-dead_strip -lpthread -o /tmp/test_mutex_completion
 *   && /tmp/test_mutex_completion
 */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/jiffies.h>

void msleep(unsigned int ms) { usleep(ms * 1000); }

static struct mutex contested;
static int count;
static void *increment(void *unused)
{
	(void)unused;
	for (int i = 0; i < 500; i++) {
		mutex_lock(&contested);
		count++;
		mutex_unlock(&contested);
	}
	return NULL;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	assert(clock_gettime(CLOCK_UPTIME_RAW, &ts) == 0);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void *finish_after_delay(void *opaque)
{
	usleep(5000);
	complete(opaque);
	return NULL;
}

static void *wait_for_all(void *opaque)
{
	assert(wait_for_completion_timeout(opaque, 100) > 0);
	return NULL;
}

int main(void)
{
	struct mutex *many = calloc(4096, sizeof(*many));
	struct completion *completions = calloc(2048, sizeof(*completions));
	struct completion c;
	pthread_t workers[6], waiters[4];
	uint64_t start, elapsed;
	unsigned long remaining;

	assert(many && completions);
	for (int i = 0; i < 4096; i++)
		mutex_init(&many[i]);
	mutex_lock(&many[0]);
	assert(mutex_is_locked(&many[0]));
	assert(mutex_trylock(&many[64])); /* same low address bits in old table */
	mutex_unlock(&many[64]);
	mutex_unlock(&many[0]);
	for (int i = 0; i < 4096; i++) {
		assert(mutex_trylock(&many[i]));
		mutex_unlock(&many[i]);
		mutex_destroy(&many[i]);
	}
	free(many);
	mutex_init(&contested);
	for (int i = 0; i < 6; i++)
		assert(pthread_create(&workers[i], NULL, increment, NULL) == 0);
	for (int i = 0; i < 6; i++)
		assert(pthread_join(workers[i], NULL) == 0);
	assert(count == 3000);
	mutex_destroy(&contested);

	for (int i = 0; i < 2048; i++)
		init_completion(&completions[i]);
	complete(&completions[0]);
	assert(try_wait_for_completion(&completions[0]));
	assert(!try_wait_for_completion(&completions[0]));
	for (int i = 1; i < 2048; i++)
		assert(!completion_done(&completions[i]));
	free(completions);

	init_completion(&c);
	start = now_ns();
	assert(wait_for_completion_timeout(&c, msecs_to_jiffies(20)) == 0);
	elapsed = now_ns() - start;
	assert(elapsed >= 19000000ULL && elapsed < 300000000ULL);
	assert(HZ == 1000);
	assert(pthread_create(&workers[0], NULL, finish_after_delay, &c) == 0);
	start = now_ns();
	remaining = wait_for_completion_timeout(&c, msecs_to_jiffies(100));
	elapsed = now_ns() - start;
	assert(remaining >= 1 && remaining <= msecs_to_jiffies(100));
	/* complete() 5 ms in woke it: no backstop or poll step on top. */
	assert(elapsed < 9000000ULL);
	assert(pthread_join(workers[0], NULL) == 0);
	assert(!completion_done(&c));

	for (int i = 0; i < 4; i++)
		assert(pthread_create(&waiters[i], NULL, wait_for_all, &c) == 0);
	usleep(5000);
	complete_all(&c);
	for (int i = 0; i < 4; i++)
		assert(pthread_join(waiters[i], NULL) == 0);
	assert(completion_done(&c));
	assert(try_wait_for_completion(&c));
	assert(completion_done(&c));
	reinit_completion(&c);
	assert(!completion_done(&c));
	assert(wait_for_completion_timeout(&c, 0) == 0);
	init_completion_done(&c);
	assert(try_wait_for_completion(&c));
	assert(!try_wait_for_completion(&c));
	complete_done(&c);
	assert(wait_for_completion(&c) == 0);
	assert(!completion_done(&c));
	return 0;
}
