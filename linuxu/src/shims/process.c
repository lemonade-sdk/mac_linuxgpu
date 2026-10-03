/* linuxu processes: a leader task, an mm and a files_struct, plus per-call
 * thread tasks that run on whatever OS thread enters the process.
 *
 * Reference ownership mirrors Linux CLONE_VM | CLONE_FILES threads: the
 * leader holds the initial mm_users and files references, and every thread
 * task holds its own while it runs. A thread task holds a reference on the
 * leader through group_leader. linuxu_process_exit is do_exit for the whole
 * group: it kills, drains, runs exit_mm's notifier release and exit_files,
 * then drops what the process holds.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/fdtable.h>
#include <linux/bug.h>
#include <rt/task.h>
#include <rt/process.h>

struct linuxu_process {
	pthread_mutex_t lock;
	pthread_cond_t idle;
	struct task_struct *leader;
	struct mm_struct *mm;
	struct files_struct *files;
	struct list_head threads;	/* running thread tasks */
	unsigned int active;
	bool killed;
	bool exiting;
};

struct linuxu_thread {
	struct task_struct task;
	struct list_head node;
};

static void release_thread_task(struct task_struct *task)
{
	struct task_struct *leader = task->group_leader;

	free(container_of(task, struct linuxu_thread, task));
	put_task_struct(leader);
}

static void release_leader_task(struct task_struct *task)
{
	free(task);
}

struct linuxu_process *linuxu_process_create(pid_t pid, const char *comm)
{
	struct linuxu_process *process = calloc(1, sizeof(*process));
	struct task_struct *leader = calloc(1, sizeof(*leader));
	struct mm_struct *mm = linuxu_mm_alloc();
	struct files_struct *files = linuxu_files_alloc();

	if (!process || !leader || !mm || !files) {
		free(process);
		free(leader);
		if (mm)
			mmput(mm);
		linuxu_files_put(files);
		return NULL;
	}
	linuxu_task_init(leader, comm, 0);
	if (pid > 0)
		leader->pid = leader->tid = leader->tgid = pid;
	leader->linuxu_release = release_leader_task;
	mm->owner = leader;
	linuxu_task_set_mm(leader, mm);
	leader->files = files;

	pthread_mutex_init(&process->lock, NULL);
	pthread_cond_init(&process->idle, NULL);
	INIT_LIST_HEAD(&process->threads);
	process->leader = leader;
	process->mm = mm;
	process->files = files;
	return process;
}

int linuxu_process_enter(struct linuxu_process *process,
			 struct linuxu_process_saved *saved)
{
	struct linuxu_thread *thread;
	struct task_struct *task, *leader;
	int ret;

	if (!process || !saved)
		return -EINVAL;
	thread = calloc(1, sizeof(*thread));
	if (!thread)
		return -ENOMEM;
	task = &thread->task;
	pthread_mutex_lock(&process->lock);
	if (process->exiting) {
		pthread_mutex_unlock(&process->lock);
		free(thread);
		return -ESRCH;
	}
	leader = process->leader;
	linuxu_task_init(task, leader->comm, 0);
	task->tgid = leader->tgid;
	task->group_leader = get_task_struct(leader);
	task->linuxu_release = release_thread_task;
	linuxu_task_set_mm(task, mmget(process->mm));
	task->files = linuxu_files_get(process->files);
	if (process->killed)
		send_sig(SIGKILL, task, 1);
	list_add_tail(&thread->node, &process->threads);
	process->active++;
	pthread_mutex_unlock(&process->lock);

	saved->process = process;
	saved->task = task;
	saved->previous = NULL;
	ret = linuxu_task_swap_current(task, &saved->previous);
	if (ret)
		linuxu_process_leave(saved);	/* never became current */
	return ret;
}

void linuxu_process_leave(struct linuxu_process_saved *saved)
{
	struct linuxu_process *process = saved->process;
	struct task_struct *task = saved->task;
	struct linuxu_thread *thread;
	struct mm_struct *mm;
	struct files_struct *files;

	if (!process || !task)
		return;
	if (current == task)
		linuxu_task_swap_current(saved->previous, NULL);
	thread = container_of(task, struct linuxu_thread, task);
	/* exit_mm / exit_files for this thread. */
	mm = linuxu_task_set_mm(task, NULL);
	files = task->files;
	task->files = NULL;
	mmput(mm);
	linuxu_files_put(files);

	pthread_mutex_lock(&process->lock);
	list_del_init(&thread->node);
	process->active--;
	pthread_cond_broadcast(&process->idle);
	pthread_mutex_unlock(&process->lock);
	saved->task = NULL;
	put_task_struct(task);
}

static void kill_locked(struct linuxu_process *process)
{
	struct linuxu_thread *thread;

	process->killed = true;
	send_sig(SIGKILL, process->leader, 1);
	list_for_each_entry(thread, &process->threads, node)
		send_sig(SIGKILL, &thread->task, 1);
}

void linuxu_process_kill(struct linuxu_process *process)
{
	if (!process)
		return;
	pthread_mutex_lock(&process->lock);
	kill_locked(process);
	pthread_mutex_unlock(&process->lock);
}

void linuxu_process_exit(struct linuxu_process *process)
{
	struct task_struct *self = current, *leader;
	unsigned int own = 0;
	struct linuxu_thread *thread;
	struct mm_struct *mm;
	struct files_struct *files;

	if (!process)
		return;
	pthread_mutex_lock(&process->lock);
	process->exiting = true;
	kill_locked(process);
	/* Exit must come from outside the process (the dext's teardown
	 * worker), never from one of its own thread tasks. */
	list_for_each_entry(thread, &process->threads, node)
		if (&thread->task == self)
			own = 1;
	if (WARN_ON_ONCE(own)) {
		/* Teardown from inside would free the process under its own
		 * running thread; the group stays killed for a later exit. */
		process->exiting = false;
		pthread_mutex_unlock(&process->lock);
		return;
	}
	while (process->active)
		pthread_cond_wait(&process->idle, &process->lock);
	pthread_mutex_unlock(&process->lock);

	leader = process->leader;
	leader->flags |= PF_EXITING;
	mm = process->mm;
	files = process->files;

	/* exit_mm: notifier ->release (KFD's process teardown starts here). */
	linuxu_mm_exit(mm);
	/* exit_files: every descriptor's release (kfd_release, drm release). */
	linuxu_files_close_all(files);

	linuxu_task_set_mm(leader, NULL);
	leader->files = NULL;
	mmput(mm);
	linuxu_files_put(files);
	pthread_cond_destroy(&process->idle);
	pthread_mutex_destroy(&process->lock);
	free(process);
	put_task_struct(leader);
}

struct task_struct *linuxu_process_leader(struct linuxu_process *process)
{
	return process ? process->leader : NULL;
}

struct mm_struct *linuxu_process_mm(struct linuxu_process *process)
{
	return process ? process->mm : NULL;
}

struct files_struct *linuxu_process_files(struct linuxu_process *process)
{
	return process ? process->files : NULL;
}

unsigned int linuxu_process_active_threads(struct linuxu_process *process)
{
	unsigned int active;

	if (!process)
		return 0;
	pthread_mutex_lock(&process->lock);
	active = process->active;
	pthread_mutex_unlock(&process->lock);
	return active;
}
