#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(shared);
static unsigned long counter;
static void *worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < 50000; ++i) {
		spin_lock(&shared);
		++counter;
		spin_unlock(&shared);
	}
	return NULL;
}
int main(void)
{
	assert(!spin_is_locked(&shared));
	assert(spin_trylock(&shared));
	assert(spin_is_locked(&shared));
	assert(!spin_trylock(&shared));
	spin_unlock(&shared);
	assert(!raw_spin_is_locked(&shared.rlock));
	spinlock_t *locks = calloc(8192, sizeof(*locks));
	assert(locks);
	for (int i = 0; i < 8192; ++i) {
		spin_lock_init(&locks[i]);
		assert(spin_trylock(&locks[i]));
	}
	for (int i = 0; i < 8192; ++i) {
		assert(!spin_trylock(&locks[i]));
		spin_unlock(&locks[i]);
		spin_lock_init(&locks[i]);
		assert(spin_trylock(&locks[i]));
		spin_unlock(&locks[i]);
	}
	free(locks);
	pthread_t threads[4];
	for (int i = 0; i < 4; ++i)
		assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
	for (int i = 0; i < 4; ++i)
		assert(pthread_join(threads[i], NULL) == 0);
	assert(counter == 200000);
	puts("spinlock contention, independent lifetimes, reuse and trylock: PASS");
}
