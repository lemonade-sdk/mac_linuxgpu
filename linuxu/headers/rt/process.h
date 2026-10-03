/* linuxu processes: the task/mm/files triple a Linux process provides.
 *
 * A process has one leader task, one mm and one files_struct. Calls made on
 * behalf of the process run on a per-call thread task: its group_leader is
 * the leader, its tgid is the leader's pid, and it shares the mm and files
 * (each thread holds an mm_users and a files reference while it runs).
 * linuxu_process_enter installs that thread task as `current` on the calling
 * OS thread and linuxu_process_leave restores the previous task; the pair
 * nests. Runtime: linuxu/src/shims/process.c.
 */
#ifndef LINUXU_RT_PROCESS_H
#define LINUXU_RT_PROCESS_H

#include <linux/sched.h>

struct linuxu_process;
struct mm_struct;
struct files_struct;

struct linuxu_process_saved {
	struct linuxu_process *process;
	struct task_struct *task;	/* the per-call thread task */
	struct task_struct *previous;	/* current before enter */
};

/* Create a process whose leader has @pid (a counter value when pid <= 0)
 * and @comm. Returns NULL on allocation failure. */
struct linuxu_process *linuxu_process_create(pid_t pid, const char *comm);
/* Run the caller as a new thread task of @process until leave. Returns
 * -ESRCH once the process has exited, -ENOMEM or -EAGAIN on failure. A
 * thread entering a killed process starts with SIGKILL pending, as every
 * thread of a killed Linux thread group has. */
int linuxu_process_enter(struct linuxu_process *process,
			 struct linuxu_process_saved *saved);
void linuxu_process_leave(struct linuxu_process_saved *saved);
/* SIGKILL the leader and every running thread task, so interruptible and
 * killable waits return. Idempotent. */
void linuxu_process_kill(struct linuxu_process *process);
/* Tear the process down like do_exit of its last thread: kill, wait for
 * every other running thread task to leave, run ->release on all mmu
 * notifier subscriptions (exit_mm), close every descriptor (exit_files),
 * then drop the process's task, mm and files references. @process is
 * freed; the leader task lives on while others hold references to it.
 * Must be called from outside the process: from one of its own thread
 * tasks it warns and only leaves the process killed. */
void linuxu_process_exit(struct linuxu_process *process);

struct task_struct *linuxu_process_leader(struct linuxu_process *process);
struct mm_struct *linuxu_process_mm(struct linuxu_process *process);
struct files_struct *linuxu_process_files(struct linuxu_process *process);
/* Thread tasks currently inside the process. */
unsigned int linuxu_process_active_threads(struct linuxu_process *process);

#endif
