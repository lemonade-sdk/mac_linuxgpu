/* Minimal per-thread Linux task identity for in-process KMD operations. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <rt/task.h>
#include <linux/list.h>
#include <linux/percpu.h>
#include <linux/pid.h>

static int next_pid = 1;
static struct task_struct fallback_task = {
	.pid = 0, .tid = 0, .tgid = 0, .comm = "<unknown>",
	.group_leader = &fallback_task,
	.usage = REFCOUNT_INIT(REFCOUNT_SATURATE),
};

char *linuxu_get_task_comm(char *buf, size_t size, const struct task_struct *task)
{
	/* Task names are initialized before publication and remain immutable. */
	size_t length = strnlen(task->comm, TASK_COMM_LEN - 1);
	if (!size)
		return buf;
	if (length >= size)
		length = size - 1;
	memcpy(buf, task->comm, length);
	memset(buf + length, 0, size - length);
	return buf;
}

void linuxu_task_init(struct task_struct *task, const char *name,
		      unsigned int flags)
{
	int pid;
	memset(task, 0, sizeof(*task));
	refcount_set(&task->usage, 1);
	pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
	if (pid <= 0)
		pid = 0;
	task->pid = pid;
	task->tid = pid;
	task->tgid = pid;
	task->flags = flags;
	task->group_leader = task;
	if (name) {
		strncpy(task->comm, name, sizeof(task->comm) - 1);
		task->comm[sizeof(task->comm) - 1] = '\0';
	}
	/* A bare task has no address space or descriptor table, like a kernel
	 * thread; linuxu_process_create/enter (process.c) attach the process's
	 * mm and files. No authenticated credentials exist. */
	task->mm = NULL;
	task->files = NULL;
	task->cred = NULL;
}

struct task_struct *get_task_struct(struct task_struct *task)
{
	if (task && refcount_inc_not_zero(&task->usage)) return task;
	return NULL;
}

/*
 * Process identifiers. Each task owns one struct pid for its own pid number;
 * PIDTYPE_TGID (and the group types) resolve through group_leader, so a
 * linuxu process thread task answers with its leader's pid, and a task that
 * leads its own group answers both. A pid is created on first use, holds
 * the task's pid number, and is published in pid_list for
 * find_get_pid(). The task owns one reference. When the task is released,
 * pid->task is cleared under pid_lock before the task memory goes away, so
 * get_pid_task() either takes a task reference or returns NULL.
 */
static pthread_mutex_t pid_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(pid_list);
static struct pid fallback_pid = {
	.count = REFCOUNT_INIT(REFCOUNT_SATURATE),
	.nr = 0,
	.task = &fallback_task,
	.node = LIST_HEAD_INIT(fallback_pid.node),
};

static struct pid *task_pid_locked(struct task_struct *task)
{
	struct pid *pid;

	if (task == &fallback_task)
		return &fallback_pid;
	if (task->thread_pid)
		return task->thread_pid;
	pid = calloc(1, sizeof(*pid));
	if (!pid)
		return NULL;
	refcount_set(&pid->count, 1);	/* owned by the task */
	pid->nr = task->pid;
	pid->task = task;
	list_add_tail(&pid->node, &pid_list);
	task->thread_pid = pid;
	return pid;
}

static struct pid *task_pid_get(struct task_struct *task, int type)
{
	struct pid *pid;

	if (!task)
		return NULL;
	if (type != PIDTYPE_PID && task->group_leader)
		task = task->group_leader;
	pthread_mutex_lock(&pid_lock);
	pid = task_pid_locked(task);
	pthread_mutex_unlock(&pid_lock);
	return pid;
}

struct pid *task_tgid(struct task_struct *tsk)
{
	return task_pid_get(tsk, PIDTYPE_TGID);
}

struct pid *get_pid(struct pid *pid)
{
	if (pid)
		refcount_inc(&pid->count);
	return pid;
}

void put_pid(struct pid *pid)
{
	if (!pid || !refcount_dec_and_test(&pid->count))
		return;
	pthread_mutex_lock(&pid_lock);
	list_del(&pid->node);
	pthread_mutex_unlock(&pid_lock);
	free(pid);
}

struct pid *get_task_pid(struct task_struct *task, int type)
{
	return get_pid(task_pid_get(task, type));
}

struct task_struct *pid_task(struct pid *pid, int type)
{
	struct task_struct *task;

	(void)type;
	if (!pid)
		return NULL;
	pthread_mutex_lock(&pid_lock);
	task = pid->task;
	pthread_mutex_unlock(&pid_lock);
	return task;
}

struct task_struct *get_pid_task(struct pid *pid, int type)
{
	struct task_struct *task = NULL;

	(void)type;
	if (!pid)
		return NULL;
	pthread_mutex_lock(&pid_lock);
	if (pid->task)
		task = get_task_struct(pid->task);
	pthread_mutex_unlock(&pid_lock);
	return task;
}

struct pid *find_get_pid(pid_t nr)
{
	struct pid *pid, *found = NULL;

	pthread_mutex_lock(&pid_lock);
	list_for_each_entry(pid, &pid_list, node) {
		if (pid->nr == nr && pid->task) {
			found = get_pid(pid);
			break;
		}
	}
	pthread_mutex_unlock(&pid_lock);
	return found;
}

pid_t pid_vnr(struct pid *pid)
{
	return pid ? pid->nr : 0;
}

int task_pid_vnr(const struct task_struct *task)
{
	return task ? task->pid : 0;
}

/* Per-CPU copies owned by a task (linux/percpu.h). A task only ever looks
 * up its own copies; a task shared by several threads (an adopted process
 * task) may insert concurrently, so insertion publishes with a CAS and
 * lookups read the list with acquire loads. Copies live until the task is
 * released. */
struct linuxu_percpu_copy {
	struct linuxu_percpu_copy *next;
	const void *var;
	size_t size;
	unsigned char data[] __attribute__((aligned(16)));
};

unsigned long linuxu_percpu_alloc_failures;

void *linuxu_this_cpu_ptr(const void *var, size_t size)
{
	struct task_struct *task = linuxu_current_task();
	struct linuxu_percpu_copy *head, *copy;

	head = __atomic_load_n((struct linuxu_percpu_copy **)&task->linuxu_percpu,
			       __ATOMIC_ACQUIRE);
	for (copy = head; copy; copy = copy->next)
		if (copy->var == var)
			return copy->data;
	copy = malloc(sizeof(*copy) + size);
	if (!copy) {
		/* Out of memory: fall back to the shared boot copy, the
		 * single-CPU behaviour, rather than fail an accessor that
		 * cannot report errors. */
		__atomic_add_fetch(&linuxu_percpu_alloc_failures, 1, __ATOMIC_RELAXED);
		return (void *)var;
	}
	copy->var = var;
	copy->size = size;
	memcpy(copy->data, var, size);
	do {
		struct linuxu_percpu_copy *seen;
		for (seen = head; seen; seen = seen->next) {
			if (seen->var == var) {
				free(copy);
				return seen->data;
			}
		}
		copy->next = head;
	} while (!__atomic_compare_exchange_n(
			(struct linuxu_percpu_copy **)&task->linuxu_percpu, &head,
			copy, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	return copy->data;
}

void linuxu_percpu_task_release(struct task_struct *task)
{
	struct linuxu_percpu_copy *copy =
		__atomic_exchange_n((struct linuxu_percpu_copy **)&task->linuxu_percpu,
				    NULL, __ATOMIC_ACQ_REL);

	while (copy) {
		struct linuxu_percpu_copy *next = copy->next;
		free(copy);
		copy = next;
	}
}

static void task_detach_pid(struct task_struct *task)
{
	struct pid *pid;

	pthread_mutex_lock(&pid_lock);
	pid = task->thread_pid;
	task->thread_pid = NULL;
	if (pid)
		pid->task = NULL;
	pthread_mutex_unlock(&pid_lock);
	put_pid(pid);
}

void put_task_struct(struct task_struct *task)
{
	if (task && refcount_dec_and_test(&task->usage)) {
		task_detach_pid(task);
		linuxu_percpu_task_release(task);
		if (task->linuxu_release)
			task->linuxu_release(task);
	}
}
static void release_allocated_task(struct task_struct *task) { free(task); }
void linuxu_task_wake(struct task_struct *task)
{
	if (!task) return;
	__atomic_store_n(&task->state, 0, __ATOMIC_RELEASE);
	__atomic_add_fetch(&task->wake_sequence, 1, __ATOMIC_RELEASE);
}
int send_sig(int sig, struct task_struct *task, int privileged)
{
	(void)privileged;
	if (!task || task == &fallback_task) return -ESRCH;
	if (sig <= 0 || sig >= (int)(sizeof(unsigned long) * 8)) return -EINVAL;
	__atomic_fetch_or(&task->pending_signals, 1UL << sig, __ATOMIC_RELEASE);
	wake_up_process(task);
	return 0;
}
int send_signal(int sig, struct task_struct *task) { return send_sig(sig, task, 0); }
bool linuxu_task_fatal_signal_pending(struct task_struct *task)
{
	return task && (__atomic_load_n(&task->pending_signals, __ATOMIC_ACQUIRE) &
		(1UL << SIGKILL));
}
void linuxu_task_clear_signals(struct task_struct *task)
{
	if (task) __atomic_store_n(&task->pending_signals, 0, __ATOMIC_RELEASE);
}

#ifdef LINUXU_DEXT_DK
static pthread_once_t task_key_once = PTHREAD_ONCE_INIT;
static uint64_t task_key;
static int task_key_ready;

extern int IOThreadLocalStorageKeyCreate(uint64_t *key);
extern int IOThreadLocalStorageSet(uint64_t key, const void *value);
extern void *IOThreadLocalStorageGet(uint64_t key);

static void task_key_init(void)
{
	task_key_ready = IOThreadLocalStorageKeyCreate(&task_key) == 0;
}

static struct task_struct *task_slot_get(void)
{
	if (pthread_once(&task_key_once, task_key_init) || !task_key_ready)
		return NULL;
	return IOThreadLocalStorageGet(task_key);
}

static int task_slot_set(struct task_struct *task)
{
	if (pthread_once(&task_key_once, task_key_init) || !task_key_ready)
		return -1;
	return IOThreadLocalStorageSet(task_key, task);
}

int linuxu_task_swap_current(struct task_struct *task,
			    struct task_struct **previous)
{
	struct task_struct *old = task_slot_get();
	if (task_slot_set(task))
		return -EAGAIN;
	if (previous) *previous = old;
	return 0;
}

struct task_struct *linuxu_current_task_peek(void)
{
	return task_slot_get();
}

struct task_struct *linuxu_current_task(void)
{
	struct task_struct *task = task_slot_get();
	if (task)
		return task;
	task = calloc(1, sizeof(*task));
	if (!task)
		return &fallback_task;
	linuxu_task_init(task, "mac_linuxgpu", 0);
	task->linuxu_release = release_allocated_task;
	if (task_slot_set(task)) {
		free(task);
		return &fallback_task;
	}
	return task;
}
void linuxu_task_cleanup_current(void)
{
	struct task_struct *task = task_slot_get();
	if (task && !(task->flags & PF_KTHREAD) && !task_slot_set(NULL))
		put_task_struct(task);
}
#else
static pthread_once_t host_task_once = PTHREAD_ONCE_INIT;
static pthread_key_t host_task_key;
static int host_task_ready;
static _Thread_local struct task_struct *thread_override;
static void host_task_destroy(void *task) { put_task_struct(task); }
static void host_task_init(void)
{ host_task_ready = !pthread_key_create(&host_task_key, host_task_destroy); }

int linuxu_task_swap_current(struct task_struct *task,
			    struct task_struct **previous)
{
	struct task_struct *old = thread_override;
	thread_override = task;
	if (previous) *previous = old;
	return 0;
}

struct task_struct *linuxu_current_task_peek(void)
{
	if (thread_override)
		return thread_override;
	if (!host_task_ready)
		return NULL;
	return pthread_getspecific(host_task_key);
}

struct task_struct *linuxu_current_task(void)
{
	if (thread_override)
		return thread_override;
	if (pthread_once(&host_task_once, host_task_init) || !host_task_ready)
		return &fallback_task;
	struct task_struct *task = pthread_getspecific(host_task_key);
	if (task) return task;
	task = calloc(1, sizeof(*task));
	if (!task) return &fallback_task;
	linuxu_task_init(task, "mac_linuxgpu", 0);
	task->linuxu_release = release_allocated_task;
	if (pthread_setspecific(host_task_key, task)) { free(task); return &fallback_task; }
	return task;
}
void linuxu_task_cleanup_current(void)
{
	if (!host_task_ready) return;
	struct task_struct *task = pthread_getspecific(host_task_key);
	if (task && !pthread_setspecific(host_task_key, NULL)) put_task_struct(task);
}
#endif
