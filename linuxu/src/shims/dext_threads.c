/* DriverKit pthread subset.  A worker is an asynchronous dispatch on its
 * own serial IODispatchQueue, not a libSystem pthread.  Long-running work
 * occupies that queue until it returns; callers must not join from the same
 * worker.  The bridge in dext_threads.mm owns queue creation and release.
 */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_TEST_DEXT_THREADS)

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef LINUXU_TEST_DEXT_THREADS
#define pthread_create dext_test_pthread_create
#define pthread_join dext_test_pthread_join
#define pthread_self dext_test_pthread_self
#define pthread_equal dext_test_pthread_equal
#endif

pthread_t pthread_self(void);
int pthread_join(pthread_t thread, void **result);

#define DEXT_THREAD_SLOTS 128

struct dext_thread_start {
	pthread_cond_t changed;
	int acknowledged;
	int error;
};
struct dext_thread_task {
	uint64_t identity;
	void *(*start)(void *);
	void *argument;
	void *result;
	pthread_cond_t complete;
	int run_error;
	int finished;
	int joining;
	struct dext_thread_start *startup;
};

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dext_thread_task *thread_slots[DEXT_THREAD_SLOTS];
static uint64_t next_generation = 1;
static pthread_once_t identity_once = PTHREAD_ONCE_INIT;
static uint64_t identity_key;
static int identity_ready;

extern int IOThreadLocalStorageKeyCreate(uint64_t *key);
extern int IOThreadLocalStorageSet(uint64_t key, const void *value);
extern void *IOThreadLocalStorageGet(uint64_t key);
extern int dext_thread_schedule(void *task, uint64_t identity);

static void identity_init(void)
{
	identity_ready = IOThreadLocalStorageKeyCreate(&identity_key) == 0;
}

static uint64_t make_identity(unsigned int slot)
{
	uint64_t generation = next_generation++;
	if (!generation || generation > (UINT64_MAX >> 8))
		return 0;
	return (generation << 8) | slot;
}

/* Called by the DriverKit callback.  The previous identity is restored when
 * the callback exits, so a reused dispatch worker keeps its parent identity. */
int dext_thread_identity_swap(uint64_t identity, uint64_t *previous)
{
	if (!previous || pthread_once(&identity_once, identity_init) ||
	    !identity_ready)
		return EAGAIN;
	*previous = (uint64_t)(uintptr_t)IOThreadLocalStorageGet(identity_key);
	return IOThreadLocalStorageSet(identity_key,
				      (const void *)(uintptr_t)identity) ? EAGAIN : 0;
}

void dext_thread_task_run(void *opaque)
{
	struct dext_thread_task *task = opaque;
	pthread_mutex_lock(&thread_lock);
	struct dext_thread_start *startup = task->startup;
	task->startup = NULL;
	startup->acknowledged = 1;
	pthread_cond_broadcast(&startup->changed);
	pthread_mutex_unlock(&thread_lock);
	void *result = task->start(task->argument);
#ifdef LINUXU_DEXT_DK
	extern void linuxu_task_cleanup_current(void);
	linuxu_task_cleanup_current();
#endif

	pthread_mutex_lock(&thread_lock);
	task->result = result;
	task->finished = 1;
	pthread_cond_broadcast(&task->complete);
	pthread_mutex_unlock(&thread_lock);
	/* A joiner may free task immediately after this unlock. */
}

void dext_thread_task_fail(void *opaque)
{
	struct dext_thread_task *task = opaque;
	pthread_mutex_lock(&thread_lock);
	struct dext_thread_start *startup = task->startup;
	task->startup = NULL;
	startup->error = EAGAIN;
	startup->acknowledged = 1;
	pthread_cond_broadcast(&startup->changed);
	task->result = NULL;
	task->run_error = EAGAIN;
	task->finished = 1;
	pthread_cond_broadcast(&task->complete);
	pthread_mutex_unlock(&thread_lock);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
		   void *(*start)(void *), void *argument)
{
	struct dext_thread_task *task;
	uint64_t identity;
	unsigned int slot;
	int r;
	struct dext_thread_start startup = { .changed = PTHREAD_COND_INITIALIZER };

	if (!thread || !start)
		return EINVAL;
	if (attributes)
		return ENOTSUP;
	if (pthread_once(&identity_once, identity_init) || !identity_ready)
		return EAGAIN;
	task = calloc(1, sizeof(*task));
	if (!task)
		return ENOMEM;
	r = pthread_cond_init(&task->complete, NULL);
	if (r) {
		free(task);
		return r;
	}
	task->start = start;
	task->argument = argument;
	task->startup = &startup;
	r = pthread_mutex_lock(&thread_lock);
	if (r) {
		pthread_cond_destroy(&task->complete);
		free(task);
		return r;
	}
	for (slot = 0; slot < DEXT_THREAD_SLOTS && thread_slots[slot]; slot++)
		;
	if (slot == DEXT_THREAD_SLOTS) {
		pthread_mutex_unlock(&thread_lock);
		pthread_cond_destroy(&task->complete);
		free(task);
		return EAGAIN;
	}
	task->identity = make_identity(slot + 1);
	if (!task->identity) {
		pthread_mutex_unlock(&thread_lock);
		pthread_cond_destroy(&task->complete);
		free(task);
		return EAGAIN;
	}
	identity = task->identity;
	thread_slots[slot] = task;
	pthread_mutex_unlock(&thread_lock);
	r = dext_thread_schedule(task, identity);
	if (r) {
		pthread_mutex_lock(&thread_lock);
		thread_slots[slot] = NULL;
		pthread_mutex_unlock(&thread_lock);
		pthread_cond_destroy(&task->complete);
		free(task);
		return r;
	}
	/* The callback has installed its dispatch identity before acknowledging.
	 * Keep this state outside task: a concurrent join may already free task. */
	pthread_mutex_lock(&thread_lock);
	while (!startup.acknowledged)
		pthread_cond_wait(&startup.changed, &thread_lock);
	r = startup.error;
	pthread_mutex_unlock(&thread_lock);
	pthread_cond_destroy(&startup.changed);
	if (r) {
		(void)pthread_join((pthread_t)(uintptr_t)identity, NULL);
		return r;
	}
	*thread = (pthread_t)(uintptr_t)identity;
	return 0;
}

int pthread_join(pthread_t thread, void **result)
{
	uint64_t identity = (uint64_t)(uintptr_t)thread;
	struct dext_thread_task *task = NULL;
	unsigned int slot = identity & 0xffu;
	int r;

	if (!identity || !slot || slot > DEXT_THREAD_SLOTS)
		return ESRCH;
	if (thread == pthread_self())
		return EDEADLK;
	r = pthread_mutex_lock(&thread_lock);
	if (r)
		return r;
	task = thread_slots[slot - 1];
	if (!task || task->identity != identity) {
		pthread_mutex_unlock(&thread_lock);
		return ESRCH;
	}
	if (task->joining) {
		pthread_mutex_unlock(&thread_lock);
		return EINVAL;
	}
	task->joining = 1;
	while (!task->finished) {
		r = pthread_cond_wait(&task->complete, &thread_lock);
		if (r) {
			task->joining = 0;
			pthread_mutex_unlock(&thread_lock);
			return r;
		}
	}
	if (result)
		*result = task->result;
	r = task->run_error;
	thread_slots[slot - 1] = NULL;
	pthread_mutex_unlock(&thread_lock);
	pthread_cond_destroy(&task->complete);
	free(task);
	return r;
}

pthread_t pthread_self(void)
{
	uint64_t identity;
	if (pthread_once(&identity_once, identity_init) || !identity_ready)
		return (pthread_t)0;
	identity = (uint64_t)(uintptr_t)IOThreadLocalStorageGet(identity_key);
	if (!identity) {
		if (pthread_mutex_lock(&thread_lock))
			return (pthread_t)0;
		identity = make_identity(0xffu);
		pthread_mutex_unlock(&thread_lock);
		if (!identity || IOThreadLocalStorageSet(identity_key,
					     (const void *)(uintptr_t)identity))
			return (pthread_t)0;
	}
	return (pthread_t)(uintptr_t)identity;
}

int pthread_equal(pthread_t first, pthread_t second)
{
	/* Handles encode a generation and slot, including identities assigned
	 * to dispatch callbacks outside pthread_create. Never dereference them. */
	return (uintptr_t)first == (uintptr_t)second;
}

#endif
