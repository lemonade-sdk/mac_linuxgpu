/* Cross-thread RCU grace period, nesting, callback, and barrier test. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/rcupdate.h>
#include <linux/list.h>

#ifdef LINUXU_DEXT_DK
static _Thread_local void *mock_tls;
int IOThreadLocalStorageKeyCreate(uint64_t *key) { *key = 1; return 0; }
int IOThreadLocalStorageSet(uint64_t key, const void *value)
{ assert(key == 1); mock_tls = (void *)value; return 0; }
void *IOThreadLocalStorageGet(uint64_t key)
{ assert(key == 1); return mock_tls; }
#endif

struct rcu_obj {
	struct rcu_head head;
	int *freed;
};

static void free_callback(struct rcu_head *head)
{
	struct rcu_obj *obj = container_of(head, struct rcu_obj, head);
	__atomic_fetch_add(obj->freed, 1, __ATOMIC_SEQ_CST);
	free(obj);
}

static void queue_one(int *freed)
{
	struct rcu_obj *obj = calloc(1, sizeof(*obj));
	assert(obj);
	obj->freed = freed;
	call_rcu(&obj->head, free_callback);
}

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int reader_stage;
static int sync_finished;

static void *reader(void *unused)
{
	(void)unused;
	rcu_read_lock();
	rcu_read_lock();
	assert(rcu_read_lock_count() == 2);
	assert(rcu_read_lock_held());
	pthread_mutex_lock(&gate);
	reader_stage = 1;
	pthread_cond_broadcast(&changed);
	while (reader_stage == 1)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	rcu_read_unlock();
	assert(rcu_read_lock_count() == 1);
	pthread_mutex_lock(&gate);
	reader_stage = 3;
	pthread_cond_broadcast(&changed);
	while (reader_stage == 3)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	rcu_read_unlock();
	assert(!rcu_read_lock_held());
	return NULL;
}

static void *synchronizer(void *unused)
{
	(void)unused;
	synchronize_rcu();
	__atomic_store_n(&sync_finished, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void *producer(void *opaque)
{
	int *freed = opaque;
	for (int i = 0; i < 200; i++)
		queue_one(freed);
	return NULL;
}

/* Lockless readers against grace periods: readers dereference the
 * published object while writers replace it and free the old one after a
 * grace period (call_rcu or synchronize_rcu), poisoning it first. A reader
 * that ever sees poison held a reference across a grace period. */
struct boxed {
	struct rcu_head head;
	uint64_t value;
};
#define BOX_POISON 0xdeaddeaddeaddeadull
static struct boxed *published_box;
static int stress_stop;
static uint64_t stress_reads;

static void box_free(struct rcu_head *head)
{
	struct boxed *b = container_of(head, struct boxed, head);

	__atomic_store_n(&b->value, BOX_POISON, __ATOMIC_SEQ_CST);
	free(b);
}

static void *stress_reader(void *unused)
{
	(void)unused;
	while (!__atomic_load_n(&stress_stop, __ATOMIC_SEQ_CST)) {
		rcu_read_lock();
		struct boxed *b = rcu_dereference(published_box);
		for (int spin = 0; spin < 64; spin++)
			assert(__atomic_load_n(&b->value, __ATOMIC_SEQ_CST) != BOX_POISON);
		rcu_read_lock();	/* nested sections keep the outer epoch */
		assert(__atomic_load_n(&b->value, __ATOMIC_SEQ_CST) != BOX_POISON);
		rcu_read_unlock();
		rcu_read_unlock();
		__atomic_add_fetch(&stress_reads, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

static void *stress_writer(void *arg)
{
	const int synchronous = (int)(intptr_t)arg;

	for (int i = 0; i < 2000; i++) {
		struct boxed *b = calloc(1, sizeof(*b));
		assert(b);
		b->value = (uint64_t)i;
		struct boxed *old = __atomic_exchange_n(&published_box, b, __ATOMIC_SEQ_CST);
		if (synchronous) {
			synchronize_rcu();
			box_free(&old->head);
		} else {
			call_rcu(&old->head, box_free);
		}
	}
	return NULL;
}

static void stress_lockless_readers(void)
{
	pthread_t readers[6], writers[2];
	struct boxed *first = calloc(1, sizeof(*first));

	assert(first);
	rcu_assign_pointer(published_box, first);
	for (int i = 0; i < 6; i++)
		assert(pthread_create(&readers[i], NULL, stress_reader, NULL) == 0);
	for (int i = 0; i < 2; i++)
		assert(pthread_create(&writers[i], NULL, stress_writer, (void *)(intptr_t)i) == 0);
	for (int i = 0; i < 2; i++)
		assert(pthread_join(writers[i], NULL) == 0);
	__atomic_store_n(&stress_stop, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < 6; i++)
		assert(pthread_join(readers[i], NULL) == 0);
	rcu_barrier();
	assert(__atomic_load_n(&stress_reads, __ATOMIC_RELAXED) > 0);
	free(published_box);
}

int main(void)
{
	pthread_t r, s, producers[4];
	int freed = 0;
	int value = 7, *published = NULL;
	struct list_head list = LIST_HEAD_INIT(list), entry = {0};
	RCU_INIT_POINTER(published, &value);
	assert(rcu_dereference(published) == &value);
	rcu_assign_pointer(published, NULL);
	assert(rcu_dereference(published) == NULL);
	list_add_rcu(&entry, &list);
	assert(list.next == &entry);
	list_del_rcu(&entry);
	assert(list.next == &list && list.prev == &list);
	assert(rcu_spawn_gp_kthread() == 0);
	assert(pthread_create(&r, NULL, reader, NULL) == 0);
	pthread_mutex_lock(&gate);
	while (reader_stage != 1)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	queue_one(&freed);
	assert(pthread_create(&s, NULL, synchronizer, NULL) == 0);
	usleep(20000);
	assert(__atomic_load_n(&freed, __ATOMIC_SEQ_CST) == 0);
	assert(!__atomic_load_n(&sync_finished, __ATOMIC_SEQ_CST));
	pthread_mutex_lock(&gate);
	reader_stage = 2;
	pthread_cond_broadcast(&changed);
	while (reader_stage != 3)
		pthread_cond_wait(&changed, &gate);
	pthread_mutex_unlock(&gate);
	usleep(10000);
	assert(__atomic_load_n(&freed, __ATOMIC_SEQ_CST) == 0);
	assert(!__atomic_load_n(&sync_finished, __ATOMIC_SEQ_CST));
	pthread_mutex_lock(&gate);
	reader_stage = 4;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&gate);
	assert(pthread_join(r, NULL) == 0);
	assert(pthread_join(s, NULL) == 0);
	assert(__atomic_load_n(&freed, __ATOMIC_SEQ_CST) == 1);
	assert(__atomic_load_n(&sync_finished, __ATOMIC_SEQ_CST));

	for (int i = 0; i < 4; i++)
		assert(pthread_create(&producers[i], NULL, producer, &freed) == 0);
	for (int i = 0; i < 4; i++)
		assert(pthread_join(producers[i], NULL) == 0);
	rcu_barrier();
	assert(__atomic_load_n(&freed, __ATOMIC_SEQ_CST) == 801);
	rcu_barrier();
	synchronize_rcu();
	stress_lockless_readers();
	return 0;
}
