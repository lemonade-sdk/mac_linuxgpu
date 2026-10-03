#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/rwsem.h>
#include <linux/semaphore.h>

void usleep_range(unsigned long min, unsigned long max)
{ (void)max; usleep((useconds_t)min); }

static struct rw_semaphore shared = RWSEM_INITIALIZER(shared);
static atomic_int writer_entered;
static atomic_int reader_entered;

static void *reader(void *unused)
{
	(void)unused;
	down_read(&shared);
	atomic_store(&reader_entered, 1);
	up_read(&shared);
	return NULL;
}

static void *writer(void *unused)
{
	(void)unused;
	down_write(&shared);
	atomic_store(&writer_entered, 1);
	up_write(&shared);
	return NULL;
}

int main(void)
{
	struct rw_semaphore many[1400] = {0};
	pthread_t thread;
	atomic_long_t value = LONGATOMIC_INIT(7);
	assert(atomic_long_cmpxchg(&value, 7, 9) == 7);
	assert(atomic_long_read(&value) == 9);
	rwlock_t reused_lock;
	memset(&reused_lock, 0xa5, sizeof(reused_lock));
	rwlock_init(&reused_lock);
	assert(down_read_trylock(&reused_lock));
	up_read(&reused_lock);
	assert(down_write_trylock(&reused_lock));
	up_write(&reused_lock);
	rwsem_destroy(&reused_lock);
	struct semaphore sem = {0};
	sema_init(&sem, 2);
	assert(down_interruptible(&sem) == 0 && sem.count == 1);
	assert(down_trylock(&sem) == 0 && sem.count == 0);
	assert(down_trylock(&sem) == 1 && sem.count == 0);
	up(&sem);
	assert(down_killable(&sem) == 0 && sem.count == 0);
	assert(atomic_long_cmpxchg(&value, 7, 11) == 9);
	assert(atomic_long_read(&value) == 9);

	for (unsigned int i = 0; i < 1400; i++) {
		init_rwsem(&many[i]);
		assert(down_write_trylock(&many[i]));
		assert(rwsem_is_locked(&many[i]));
	}
	for (unsigned int i = 0; i < 1400; i++) {
		up_write(&many[i]);
		assert(!rwsem_is_locked(&many[i]));
	}
	for (unsigned int i = 0; i < 1400; i++)
		rwsem_destroy(&many[i]);
	/* Reinitialize the same semaphore address after destruction. */
	init_rwsem(&many[0]);
	assert(down_read_trylock(&many[0]));
	up_read(&many[0]);
	rwsem_destroy(&many[0]);

	down_read(&shared);
	assert(rwsem_is_locked(&shared));
	assert(down_read_trylock(&shared));
	assert(!down_write_trylock(&shared));
	assert(pthread_create(&thread, NULL, reader, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_load(&reader_entered));
	assert(pthread_create(&thread, NULL, writer, NULL) == 0);
	for (int i = 0; i < 1000 && !rwsem_is_contended(&shared); i++)
		usleep(1000);
	assert(rwsem_is_contended(&shared));
	assert(!atomic_load(&writer_entered));
	up_read(&shared);
	up_read(&shared);
	assert(pthread_join(thread, NULL) == 0);
	assert(atomic_load(&writer_entered));
	assert(!rwsem_is_locked(&shared));
	rwsem_destroy(&shared);
	puts("allocation-free rwsem lifecycle, shared readers and writer exclusion passed");
	return 0;
}
