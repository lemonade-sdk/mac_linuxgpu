#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/kthread.h>
#include <linux/wait.h>

static int ran;
static int worker_pid;
static struct task_struct *main_task;
static int park_runs, self_parked, entered_exit_worker, seen_park, permit_exit;

static void task_name_copy(void)
{
	struct task_struct task = { .comm = "gpu-worker" };
	struct { char before; char name[TASK_COMM_LEN]; char after; } guarded;
	memset(&guarded, 0x5a, sizeof(guarded));
	assert(get_task_comm(guarded.name, &task) == guarded.name);
	assert(!strcmp(guarded.name, "gpu-worker"));
	assert(guarded.before == 0x5a && guarded.after == 0x5a);
	for (size_t i = strlen(task.comm); i < sizeof(guarded.name); i++)
		assert(!guarded.name[i]);
	char large[TASK_COMM_LEN * 2];
	memset(task.comm, 'x', TASK_COMM_LEN - 1);
	task.comm[TASK_COMM_LEN - 1] = 0;
	memset(large, 0x5a, sizeof(large));
	assert(get_task_comm(large, &task) == large);
	assert(!memcmp(large, task.comm, TASK_COMM_LEN));
	for (size_t i = TASK_COMM_LEN; i < sizeof(large); i++)
		assert(!large[i]);
}

static void await_value(int *value, int expected)
{
	for (unsigned i = 0; i < 10000; i++) {
		if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected) return;
		usleep(100);
	}
	assert(!"worker deadline expired");
}

static int parking_worker(void *unused)
{
	(void)unused;
	__atomic_add_fetch(&park_runs, 1, __ATOMIC_RELEASE);
	while (!kthread_should_stop()) {
		if (kthread_should_park()) {
			assert(kthread_should_stop_or_park());
			kthread_parkme();
			__atomic_add_fetch(&park_runs, 1, __ATOMIC_RELEASE);
		}
		usleep(100);
	}
	return 19;
}

static int self_parking_worker(void *unused)
{
	(void)unused;
	assert(!kthread_park(current));
	assert(kthread_should_park());
	__atomic_store_n(&self_parked, 1, __ATOMIC_RELEASE);
	kthread_parkme();
	assert(!kthread_should_park());
	__atomic_store_n(&self_parked, 2, __ATOMIC_RELEASE);
	return 23;
}

static int exit_while_parking(void *unused)
{
	(void)unused;
	__atomic_store_n(&entered_exit_worker, 1, __ATOMIC_RELEASE);
	while (!kthread_should_park()) usleep(100);
	__atomic_store_n(&seen_park, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&permit_exit, __ATOMIC_ACQUIRE)) usleep(100);
	return 29;
}

static void *park_thread(void *task)
{
	assert(kthread_park(task) == -ENOSYS);
	return NULL;
}

static void parking_lifecycle(void)
{
	struct task_struct *task = kthread_create(parking_worker, NULL, "park-before-run");
	assert(!IS_ERR(task));
	assert(!kthread_park(task));
	assert(!__atomic_load_n(&park_runs, __ATOMIC_ACQUIRE));
	assert(kthread_park(task) == -EBUSY);
	wake_up_process(task);
	usleep(1000);
	assert(!__atomic_load_n(&park_runs, __ATOMIC_ACQUIRE));
	assert(__atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_PARKED);
	kthread_unpark(task);
	await_value(&park_runs, 1);
	for (int i = 1; i <= 3; i++) {
		assert(!kthread_park(task));
		assert(__atomic_load_n(&task->state, __ATOMIC_ACQUIRE) == TASK_PARKED);
		kthread_unpark(task);
		await_value(&park_runs, i + 1);
	}
	assert(!kthread_park(task));
	assert(kthread_stop(task) == 19);

	task = kthread_create(parking_worker, NULL, "park-then-stop");
	assert(!IS_ERR(task) && !kthread_park(task));
	assert(kthread_stop(task) == -EINTR);

	task = kthread_run(self_parking_worker, NULL, "self-park");
	assert(!IS_ERR(task));
	await_value(&self_parked, 1);
	kthread_unpark(task);
	await_value(&self_parked, 2);
	assert(kthread_stop(task) == 23);

	for (unsigned i = 0; i < 32; i++) {
		__atomic_store_n(&entered_exit_worker, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&seen_park, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&permit_exit, 0, __ATOMIC_RELEASE);
		task = kthread_run(exit_while_parking, NULL, "park-exit-race");
		assert(!IS_ERR(task));
		/* An earlier request can validly park the trampoline before entry.
		 * This case must exercise a running thread exiting without parkme. */
		await_value(&entered_exit_worker, 1);
		pthread_t parker;
		assert(!pthread_create(&parker, NULL, park_thread, task));
		await_value(&seen_park, 1);
		__atomic_store_n(&permit_exit, 1, __ATOMIC_RELEASE);
		assert(kthread_stop(task) == 29);
		assert(!pthread_join(parker, NULL));
	}
}

static int worker(void *unused)
{
	(void)unused;
	assert(current && current != main_task);
	assert(current->group_leader == current);
	assert(current->flags & PF_KTHREAD);
	assert(current->mm == NULL);
	assert(!strcmp(current->comm, "gpu-worker"));
	__atomic_store_n(&worker_pid, current->pid, __ATOMIC_RELEASE);
	__atomic_add_fetch(&ran, 1, __ATOMIC_ACQ_REL);
	while (!kthread_should_stop())
		usleep(1000);
	return 37;
}

static void *plain_thread(void *unused)
{
	(void)unused;
	struct task_struct *one = current;
	assert(one && one == current && one != main_task);
	assert(one->pid != main_task->pid);
	assert(one->mm == NULL);
	return NULL;
}

int main(void)
{
	alarm(15);
	task_name_copy();
	struct task_struct *task;
	pthread_t thread;
	main_task = current;
	assert(main_task && main_task == current);
	assert(main_task->pid > 0 && main_task->group_leader == main_task);
	assert(main_task->mm == NULL);
	assert(pthread_create(&thread, NULL, plain_thread, NULL) == 0);
	assert(pthread_join(thread, NULL) == 0);
	task = kthread_create(worker, NULL, "gpu-%s", "worker");
	assert(!IS_ERR(task));
	usleep(5000);
	assert(!__atomic_load_n(&ran, __ATOMIC_ACQUIRE));
	wake_up_process(task);
	for (int i = 0; i < 1000 && !__atomic_load_n(&ran, __ATOMIC_ACQUIRE); i++)
		usleep(1000);
	assert(__atomic_load_n(&ran, __ATOMIC_ACQUIRE) == 1);
	assert(__atomic_load_n(&worker_pid, __ATOMIC_ACQUIRE) == task->pid);
	assert(kthread_stop(task) == 37);
	assert(current == main_task);
	task = kthread_create(worker, NULL, "never-started");
	assert(!IS_ERR(task));
	assert(kthread_stop(task) < 0);
	assert(__atomic_load_n(&ran, __ATOMIC_ACQUIRE) == 1);
	task = kthread_create_on_cpu(worker, NULL, 0, "gpu-worker");
	assert(IS_ERR(task) && PTR_ERR(task) == -EOPNOTSUPP);
	struct kthread_worker *cpu_worker = kthread_create_worker_on_cpu(0, 0, "gpu-worker");
	assert(IS_ERR(cpu_worker) && PTR_ERR(cpu_worker) == -EOPNOTSUPP);
	struct kthread_worker *frozen_worker = kthread_create_worker(KTW_FREEZABLE, "unsupported-freezer");
	assert(IS_ERR(frozen_worker) && PTR_ERR(frozen_worker) == -EOPNOTSUPP);
	parking_lifecycle();
	puts("task identity and kthread lifecycle passed");
	return 0;
}
