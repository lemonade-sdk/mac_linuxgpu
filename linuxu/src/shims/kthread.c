/* Linux kthread lifecycle over the host or DriverKit pthread adapter. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <rt/task.h>

struct linuxu_kthread {
	struct task_struct task;
	struct linuxu_kthread *next;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t start_cv;
	int (*entry)(void *);
	void *argument;
	int started;
	int stop;
	int joining;
	int result;
	int retain_task;
	int identity_ready;
	int park_requested;
	int parked;
	int exited;
};

static pthread_mutex_t kthread_list_lock = PTHREAD_MUTEX_INITIALIZER;
static struct linuxu_kthread *kthread_list;
static void release_kthread_task(struct task_struct *task)
{
	struct linuxu_kthread *k = container_of(task, struct linuxu_kthread, task);
	pthread_cond_destroy(&k->start_cv);
	pthread_mutex_destroy(&k->lock);
	free(k);
}

static struct linuxu_kthread *find_kthread(struct task_struct *task)
{
	struct linuxu_kthread *k;
	for (k = kthread_list; k; k = k->next)
		if (&k->task == task)
			return k;
	return NULL;
}

static void *kthread_entry(void *opaque)
{
	struct linuxu_kthread *k = opaque;
	struct task_struct *previous = NULL;
	if (linuxu_task_swap_current(&k->task, &previous)) {
		/* Without the task identity should_stop() cannot observe this
		 * thread's stop request. Do not enter an unjoinable worker loop. */
		k->result = -EAGAIN;
		pthread_mutex_lock(&k->lock);
		k->identity_ready = -EAGAIN;
		k->exited = 1;
		pthread_cond_broadcast(&k->start_cv);
		pthread_mutex_unlock(&k->lock);
		return NULL;
	}
	pthread_mutex_lock(&k->lock);
	k->identity_ready = 1;
	pthread_cond_broadcast(&k->start_cv);
	while (!k->started && !k->stop)
		pthread_cond_wait(&k->start_cv, &k->lock);
	pthread_mutex_unlock(&k->lock);
	kthread_parkme();
	pthread_mutex_lock(&k->lock);
	int run = !k->stop;
	pthread_mutex_unlock(&k->lock);
	k->result = run ? k->entry(k->argument) : -EINTR;
	if (linuxu_task_swap_current(previous, NULL)) {
		/* The dispatch thread can be reused with this pointer still in TLS.
		 * Preserve its task object instead of leaving a dangling identity. */
		k->retain_task = 1;
		k->result = -EAGAIN;
	}
	pthread_mutex_lock(&k->lock);
	k->exited = 1;
	pthread_cond_broadcast(&k->start_cv);
	pthread_mutex_unlock(&k->lock);
	return NULL;
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
		void *data, int node, const char *namefmt, ...)
{
	struct linuxu_kthread *k;
	char name[TASK_COMM_LEN];
	va_list args;
	int ret;
	(void)node;
	if (!threadfn || !namefmt)
		return ERR_PTR(-EINVAL);
	k = calloc(1, sizeof(*k));
	if (!k)
		return ERR_PTR(-ENOMEM);
	va_start(args, namefmt);
	vsnprintf(name, sizeof(name), namefmt, args);
	va_end(args);
	linuxu_task_init(&k->task, name, PF_KTHREAD);
	k->task.linuxu_release = release_kthread_task;
	k->entry = threadfn;
	k->argument = data;
	ret = pthread_mutex_init(&k->lock, NULL);
	if (ret) {
		free(k);
		return ERR_PTR(-ret);
	}
	ret = pthread_cond_init(&k->start_cv, NULL);
	if (ret) {
		pthread_mutex_destroy(&k->lock);
		free(k);
		return ERR_PTR(-ret);
	}
	pthread_mutex_lock(&kthread_list_lock);
	k->next = kthread_list;
	kthread_list = k;
	pthread_mutex_unlock(&kthread_list_lock);
	ret = pthread_create(&k->thread, NULL, kthread_entry, k);
	if (ret) {
		struct linuxu_kthread **link;
		pthread_mutex_lock(&kthread_list_lock);
		for (link = &kthread_list; *link; link = &(*link)->next)
			if (*link == k) {
				*link = k->next;
				break;
			}
		pthread_mutex_unlock(&kthread_list_lock);
		pthread_cond_destroy(&k->start_cv);
		pthread_mutex_destroy(&k->lock);
		free(k);
		return ERR_PTR(-ret);
	}
	pthread_mutex_lock(&k->lock);
	while (!k->identity_ready)
		pthread_cond_wait(&k->start_cv, &k->lock);
	ret = k->identity_ready;
	pthread_mutex_unlock(&k->lock);
	if (ret < 0) {
		(void)kthread_stop(&k->task);
		return ERR_PTR(ret);
	}
	return &k->task;
}

struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
		void *data, unsigned int cpu, const char *namefmt)
{
	/* A dispatch queue cannot promise Linux per-CPU execution. */
	(void)threadfn; (void)data; (void)cpu; (void)namefmt;
	return ERR_PTR(-EOPNOTSUPP);
}

void wake_up_process(struct task_struct *task)
{
	struct linuxu_kthread *k;
	if (!task)
		return;
	pthread_mutex_lock(&kthread_list_lock);
	k = find_kthread(task);
	if (k) {
		pthread_mutex_lock(&k->lock);
		if (!k->parked) linuxu_task_wake(task);
		k->started = 1;
		pthread_cond_broadcast(&k->start_cv);
		pthread_mutex_unlock(&k->lock);
	} else linuxu_task_wake(task);
	pthread_mutex_unlock(&kthread_list_lock);
}

bool kthread_should_stop(void)
{
	struct linuxu_kthread *k;
	bool stop = false;
	pthread_mutex_lock(&kthread_list_lock);
	k = find_kthread(linuxu_current_task());
	if (k)
		stop = __atomic_load_n(&k->stop, __ATOMIC_ACQUIRE) != 0;
	pthread_mutex_unlock(&kthread_list_lock);
	return stop;
}

bool kthread_should_park(void)
{
	pthread_mutex_lock(&kthread_list_lock);
	struct linuxu_kthread *k = find_kthread(current);
	bool park = k && __atomic_load_n(&k->park_requested, __ATOMIC_ACQUIRE);
	pthread_mutex_unlock(&kthread_list_lock);
	return park;
}

bool kthread_should_stop_or_park(void)
{
	return kthread_should_stop() || kthread_should_park();
}

void kthread_parkme(void)
{
	pthread_mutex_lock(&kthread_list_lock);
	struct linuxu_kthread *k = find_kthread(current);
	if (!k) { pthread_mutex_unlock(&kthread_list_lock); return; }
	pthread_mutex_lock(&k->lock);
	pthread_mutex_unlock(&kthread_list_lock);
	while (k->park_requested && !k->stop) {
		k->parked = 1;
		__atomic_store_n(&k->task.state, TASK_PARKED, __ATOMIC_RELEASE);
		pthread_cond_broadcast(&k->start_cv);
		pthread_cond_wait(&k->start_cv, &k->lock);
	}
	k->parked = 0;
	__atomic_store_n(&k->task.state, TASK_RUNNING, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&k->lock);
}

int kthread_park(struct task_struct *task)
{
	if (IS_ERR_OR_NULL(task)) return -EINVAL;
	pthread_mutex_lock(&kthread_list_lock);
	struct linuxu_kthread *k = find_kthread(task);
	if (!k) { pthread_mutex_unlock(&kthread_list_lock); return -ENOSYS; }
	/* Stop may join and retire the registry entry while we wait for the
	 * park acknowledgement. Keep its lock and condition alive until done. */
	get_task_struct(task);
	pthread_mutex_lock(&k->lock);
	bool stopping = k->joining;
	pthread_mutex_unlock(&kthread_list_lock);
	int result = 0;
	if (stopping || k->stop || k->exited) result = -ENOSYS;
	else if (k->park_requested) result = -EBUSY;
	else {
		__atomic_store_n(&k->park_requested, 1, __ATOMIC_RELEASE);
		if (task != current) {
			k->started = 1;
			linuxu_task_wake(task);
			pthread_cond_broadcast(&k->start_cv);
			while (!k->parked && k->park_requested && !k->stop && !k->exited)
				pthread_cond_wait(&k->start_cv, &k->lock);
			if (!k->parked) result = -ENOSYS;
		}
	}
	pthread_mutex_unlock(&k->lock);
	put_task_struct(task);
	return result;
}

void kthread_unpark(struct task_struct *task)
{
	pthread_mutex_lock(&kthread_list_lock);
	struct linuxu_kthread *k = find_kthread(task);
	if (k) {
		pthread_mutex_lock(&k->lock);
		__atomic_store_n(&k->park_requested, 0, __ATOMIC_RELEASE);
		pthread_cond_broadcast(&k->start_cv);
		pthread_mutex_unlock(&k->lock);
	}
	pthread_mutex_unlock(&kthread_list_lock);
}

static int stop_kthread(struct task_struct *task, bool release_caller, bool *joined)
{
	struct linuxu_kthread *k;
	struct linuxu_kthread **link;
	int ret;
	if (joined) *joined = false;
	if (!task || IS_ERR(task))
		return -EINVAL;
	pthread_mutex_lock(&kthread_list_lock);
	k = find_kthread(task);
	if (!k) {
		pthread_mutex_unlock(&kthread_list_lock);
		return -ESRCH;
	}
	if (k->joining) {
		pthread_mutex_unlock(&kthread_list_lock);
		return -EBUSY;
	}
	if (linuxu_current_task() == task) {
		pthread_mutex_unlock(&kthread_list_lock);
		return -EDEADLK;
	}
	k->joining = 1;
	pthread_mutex_lock(&k->lock);
	__atomic_store_n(&k->stop, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&k->park_requested, 0, __ATOMIC_RELEASE);
	linuxu_task_wake(task);
	pthread_cond_broadcast(&k->start_cv);
	pthread_mutex_unlock(&k->lock);
	pthread_mutex_unlock(&kthread_list_lock);
	ret = pthread_join(k->thread, NULL);
	if (ret) {
		pthread_mutex_lock(&kthread_list_lock);
		k->joining = 0;
		pthread_mutex_unlock(&kthread_list_lock);
		return -ret;
	}
	if (joined) *joined = true;
	pthread_mutex_lock(&kthread_list_lock);
	for (link = &kthread_list; *link; link = &(*link)->next)
		if (*link == k) {
			*link = k->next;
			break;
		}
	pthread_mutex_unlock(&kthread_list_lock);
	ret = k->result;
	if (!k->retain_task)
		put_task_struct(task);
	if (release_caller)
		put_task_struct(task);
	return ret;
}

int kthread_stop(struct task_struct *task) { return stop_kthread(task, false, NULL); }
int kthread_stop_put(struct task_struct *task)
{
	return stop_kthread(task, true, NULL);
}

/* Worker callbacks can free their enclosing work object. Execution records
 * and flush tickets therefore live on the worker/waiter's stack. */
static pthread_mutex_t kw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kw_changed = PTHREAD_COND_INITIALIZER;
struct kw_execution {
	struct kw_execution *next;
	struct kthread_work *work;
	struct kthread_worker *worker;
	unsigned long ticket;
	bool cancelling;
};
struct kw_barrier { struct kw_barrier *next; unsigned long running, pending; };
static struct kw_execution *kw_running;
static struct kw_barrier *kw_barriers;
static unsigned long kw_ticket;
static struct kw_execution *kw_find(struct kthread_work *work)
{
	for (struct kw_execution *run = kw_running; run; run = run->next)
		if (run->work == work) return run;
	return NULL;
}
static void kw_finish(unsigned long ticket)
{
	for (struct kw_barrier *barrier = kw_barriers; barrier; barrier = barrier->next) {
		if (barrier->running == ticket) barrier->running = 0;
		if (barrier->pending == ticket) barrier->pending = 0;
	}
	pthread_cond_broadcast(&kw_changed);
}
static int kw_main(void *opaque)
{
	struct kthread_worker *worker = opaque;
	pthread_mutex_lock(&kw_lock);
	for (;;) {
		if (kthread_should_park()) {
			pthread_mutex_unlock(&kw_lock);
			kthread_parkme();
			pthread_mutex_lock(&kw_lock);
		}
		if (!list_empty(&worker->work_list)) {
			struct kthread_work *work = list_first_entry(&worker->work_list,
				struct kthread_work, node);
			struct kw_execution run = { .next = kw_running, .work = work,
				.worker = worker, .ticket = work->queue_seq };
			void (*function)(struct kthread_work *) = work->func;
			list_del_init(&work->node);
			kw_running = &run;
			worker->current_work = work;
			pthread_mutex_unlock(&kw_lock);
			function(work);
			pthread_mutex_lock(&kw_lock);
			struct kw_execution **link = &kw_running;
			while (*link != &run) link = &(*link)->next;
			*link = run.next;
			worker->current_work = NULL;
			kw_finish(run.ticket);
			continue;
		}
		if (worker->closing || kthread_should_stop()) break;
		struct timespec tick = { .tv_nsec = 1000000 };
		pthread_cond_timedwait_relative_np(&kw_changed, &kw_lock, &tick);
	}
	pthread_cond_broadcast(&kw_changed);
	pthread_mutex_unlock(&kw_lock);
	return 0;
}
static struct kthread_worker *kw_create(unsigned int flags, int node,
	const char *namefmt, va_list args, bool start)
{
	if (!namefmt) return ERR_PTR(-EINVAL);
	/* Freezer integration requires a host suspend/resume protocol. */
	if (flags) return ERR_PTR(-EOPNOTSUPP);
	struct kthread_worker *worker = calloc(1, sizeof(*worker));
	if (!worker) return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&worker->work_list);
	char name[TASK_COMM_LEN];
	vsnprintf(name, sizeof(name), namefmt, args);
	worker->task = kthread_create_on_node(kw_main, worker, node, "%s", name);
	if (IS_ERR(worker->task)) {
		int ret = PTR_ERR(worker->task);
		free(worker);
		return ERR_PTR(ret);
	}
	if (start) wake_up_process(worker->task);
	return worker;
}
struct kthread_worker *kthread_create_worker_on_node(unsigned int flags,
	int node, const char *namefmt, ...)
{
	va_list args; va_start(args, namefmt);
	struct kthread_worker *worker = kw_create(flags, node, namefmt, args, false);
	va_end(args); return worker;
}
struct kthread_worker *kthread_create_worker(unsigned int flags,
	const char *namefmt, ...)
{
	va_list args; va_start(args, namefmt);
	struct kthread_worker *worker = kw_create(flags, -1, namefmt, args, false);
	va_end(args); return worker;
}
struct kthread_worker *kthread_create_worker_on_cpu(int cpu, unsigned int flags,
	const char *namefmt)
{
	(void)cpu; (void)flags; (void)namefmt;
	return ERR_PTR(-EOPNOTSUPP);
}
struct kthread_worker *kthread_run_worker(unsigned int flags,
	const char *namefmt, ...)
{
	va_list args; va_start(args, namefmt);
	struct kthread_worker *worker = kw_create(flags, -1, namefmt, args, true);
	va_end(args); return worker;
}
void kthread_init_work(struct kthread_work *work,
	void (*function)(struct kthread_work *))
{
	memset(work, 0, sizeof(*work));
	INIT_LIST_HEAD(&work->node);
	work->func = function;
}
bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
	if (IS_ERR_OR_NULL(worker) || !work || !work->func) return false;
	bool queued = false;
	pthread_mutex_lock(&kw_lock);
	struct kw_execution *run = kw_find(work);
	if (!worker->closing && list_empty(&work->node) &&
	    (!run || (!run->cancelling && run->worker == worker))) {
		work->worker = worker;
		if (!++kw_ticket) ++kw_ticket;
		work->queue_seq = kw_ticket;
		list_add_tail(&work->node, &worker->work_list);
		queued = true;
		pthread_cond_broadcast(&kw_changed);
	}
	pthread_mutex_unlock(&kw_lock);
	return queued;
}
void kthread_flush_work(struct kthread_work *work)
{
	pthread_mutex_lock(&kw_lock);
	struct kw_execution *run = kw_find(work);
	struct kw_barrier barrier = { .next = kw_barriers,
		.running = run ? run->ticket : 0,
		.pending = list_empty(&work->node) ? 0 : work->queue_seq };
	kw_barriers = &barrier;
	while (barrier.running || barrier.pending)
		pthread_cond_wait(&kw_changed, &kw_lock);
	struct kw_barrier **link = &kw_barriers;
	while (*link != &barrier) link = &(*link)->next;
	*link = barrier.next;
	pthread_mutex_unlock(&kw_lock);
}
static void kw_flush_marker(struct kthread_work *work) { (void)work; }
void kthread_flush_worker(struct kthread_worker *worker)
{
	struct kthread_work marker;
	kthread_init_work(&marker, kw_flush_marker);
	pthread_mutex_lock(&kw_lock);
	bool closing = worker->closing;
	if (closing) {
		while (worker->current_work || !list_empty(&worker->work_list))
			pthread_cond_wait(&kw_changed, &kw_lock);
	} else {
		/* A FIFO marker waits only for work visible when flush starts.
		 * Work submitted later must not indefinitely extend the barrier. */
		marker.worker = worker;
		if (!++kw_ticket) ++kw_ticket;
		marker.queue_seq = kw_ticket;
		list_add_tail(&marker.node, &worker->work_list);
		pthread_cond_broadcast(&kw_changed);
	}
	pthread_mutex_unlock(&kw_lock);
	if (!closing) kthread_flush_work(&marker);
}
void kthread_flush_work_sync(struct kthread_worker *worker)
{ kthread_flush_worker(worker); }
bool kthread_cancel_work_sync(struct kthread_work *work)
{
	pthread_mutex_lock(&kw_lock);
	bool pending = !list_empty(&work->node);
	if (pending) { list_del_init(&work->node); kw_finish(work->queue_seq); }
	struct kw_execution *run = kw_find(work);
	if (run) run->cancelling = true;
	while (kw_find(work)) pthread_cond_wait(&kw_changed, &kw_lock);
	pthread_mutex_unlock(&kw_lock);
	return pending;
}
void kthread_cancel_work(struct kthread_worker *worker, struct kthread_work *work)
{
	(void)worker;
	(void)kthread_cancel_work_sync(work);
}
void kthread_destroy_worker(struct kthread_worker *worker)
{
	if (IS_ERR_OR_NULL(worker) || current == worker->task) return;
	pthread_mutex_lock(&kw_lock);
	worker->closing = true;
	pthread_cond_broadcast(&kw_changed);
	pthread_mutex_unlock(&kw_lock);
	/* Wake a created-but-not-started worker so accepted work is drained. */
	kthread_unpark(worker->task);
	wake_up_process(worker->task);
	kthread_flush_worker(worker);
	bool joined;
	(void)stop_kthread(worker->task, false, &joined);
	if (joined) free(worker);
}
