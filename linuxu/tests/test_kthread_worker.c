#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/shims/kthread.c"

static atomic_int entered, permitted, calls, barrier_done;
static void wait_value(atomic_int *value, int expected)
{
	while (atomic_load(value) != expected) usleep(100);
}
static void block(struct kthread_work *work)
{
	(void)work;
	assert(current->flags & PF_KTHREAD);
	atomic_store(&entered, 1);
	wait_value(&permitted, 1);
}
static void count(struct kthread_work *work)
{ (void)work; atomic_fetch_add_explicit(&calls, 1, memory_order_seq_cst); }
static void self_free(struct kthread_work *work)
{
	block(work);
	free(work);
}
static void *flush_one(void *work)
{ kthread_flush_work(work); atomic_store(&barrier_done, 1); return NULL; }
static void *cancel_one(void *work)
{ assert(!kthread_cancel_work_sync(work)); atomic_store(&barrier_done, 1); return NULL; }
static void self_free_case(struct kthread_worker *worker, bool cancel)
{
	struct kthread_work *work = malloc(sizeof(*work));
	kthread_init_work(work, self_free);
	atomic_store(&entered, 0); atomic_store(&permitted, 0); atomic_store(&barrier_done, 0);
	assert(kthread_queue_work(worker, work));
	wait_value(&entered, 1);
	pthread_t waiter;
	assert(!pthread_create(&waiter, NULL, cancel ? cancel_one : flush_one, work));
	for (;;) {
		pthread_mutex_lock(&kw_lock);
		struct kw_execution *run = kw_find(work);
		bool waiting = cancel ? run && run->cancelling : kw_barriers != NULL;
		pthread_mutex_unlock(&kw_lock);
		if (waiting) break;
		usleep(100);
	}
	assert(!atomic_load(&barrier_done));
	atomic_store(&permitted, 1);
	assert(!pthread_join(waiter, NULL));
	kthread_flush_worker(worker);
}
int main(void)
{
	alarm(15);
	struct kthread_worker *worker = kthread_create_worker(0, "dormant-%u", 1);
	assert(!IS_ERR(worker));
	struct kthread_work work;
	kthread_init_work(&work, count);
	assert(kthread_queue_work(worker, &work));
	assert(!kthread_queue_work(worker, &work));
	assert(kthread_cancel_work_sync(&work));
	assert(!atomic_load(&calls));
	kthread_destroy_worker(worker);
	worker = kthread_run_worker(0, "active-%u", 2);
	assert(!IS_ERR(worker));
	assert(kthread_queue_work(worker, &work));
	kthread_flush_work(&work);
	assert(atomic_load(&calls) == 1);
	assert(!kthread_park(worker->task));
	assert(kthread_queue_work(worker, &work));
	usleep(1000);
	assert(atomic_load(&calls) == 1);
	kthread_unpark(worker->task);
	kthread_flush_work(&work);
	assert(atomic_load(&calls) == 2);
	self_free_case(worker, false);
	self_free_case(worker, true);
	struct task_struct *task = get_task_struct(worker->task);
	assert(task && !strcmp(task->comm, "active-2"));
	assert(!kthread_park(task));
	assert(kthread_queue_work(worker, &work));
	kthread_destroy_worker(worker);
	assert(atomic_load(&calls) == 3);
	assert(!strcmp(task->comm, "active-2"));
	put_task_struct(task);
	puts("kthread workers: dormant cancellation, real task execution, self-free barriers and retained task ownership passed");
}
