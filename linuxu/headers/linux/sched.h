/* linuxu: SHIM (third_party/linux/include/linux/sched.h)
 * task_struct surface + current. Runtime: linuxu/src/sched/sched.c
 * (self-contained; <linux/kernel.h> includes this, and kernel.h
 * includes mm.h — so no reverse includes are allowed here.)
 */
#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#include <linux/types.h>
#include <linux/capability.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/cred.h>
#include <linux/refcount.h>

struct mm_struct;
struct pid;
struct signal_struct;
struct fs_struct;
struct files_struct;
struct cred;

#define TASK_COMM_LEN	16

struct thread_struct {
	unsigned long		sp;
};
struct task_struct {
	volatile long		state;
	unsigned int		*stack;
	int			pid;
	int			tid;
	int			tgid;
	int			priority;
	char			comm[TASK_COMM_LEN];
	struct list_head	run_list;
	struct task_struct	*parent;
	struct list_head	children;
	struct list_head	siblings;
	struct task_struct	*group_leader;
	struct mm_struct	*mm;
	struct mm_struct	*active_mm;
	int			exit_code;
	int			exit_signal;
	int			pdeath_signal;
	struct pid		*thread_pid;
	struct signal_struct	*signal;
	struct fs_struct	*fs;
	struct files_struct	*files;
	struct cred		*cred;
	struct cred		*real_cred;
	struct cred		*exec_cred;
	struct thread_struct	thread;
	unsigned int		flags;
	refcount_t usage;
	void (*linuxu_release)(struct task_struct *);
	unsigned long pending_signals;
	unsigned long wake_sequence;
};

char *linuxu_get_task_comm(char *buf, size_t size, const struct task_struct *task);
#define get_task_comm(buf, task) ({ \
	(void)sizeof(char[(sizeof(buf) >= TASK_COMM_LEN) ? 1 : -1]); \
	linuxu_get_task_comm((buf), sizeof(buf), (task)); \
})

static inline const struct cred *__task_cred(const struct task_struct *task)
{
	static const struct cred unknown = { .euid = LINUXU_UNKNOWN_KUID };
	return task && task->cred ? task->cred : &unknown;
}

#define PF_KTHREAD		0x00200000
#define PF_EXITING		0x00000004
#define PF_MEMALLOC		0x00010000
#define PF_MEMALLOC_NOIO	0x00040000
#define PF_MEMALLOC_NOFS	0x00080000
#define PF_NO_SETAFFINITY	0x04000000

extern struct task_struct *linuxu_current_task(void);
#define current		linuxu_current_task()

static inline int task_pid_nr(struct task_struct *t) { return t->pid; }
static inline int task_tgid_nr(struct task_struct *t) { return t->tgid; }
static inline int task_cpu(const struct task_struct *t) { return 0; }
static inline void *task_stack_page(const struct task_struct *t) { return (void *)t->stack; }

extern void schedule(void);
extern void free_task(struct task_struct *t);
extern void exit_task(struct task_struct *tsk);
extern void exit_mm(struct mm_struct *mm);
extern void exit_fs(struct fs_struct *fs);
extern void exit_files(struct files_struct *files);
extern void exit_creds(struct cred *cred);
extern void exit_signals(struct signal_struct *sig);
extern void exit_thread(struct thread_struct *thread);
extern void exit_notify(struct task_struct *tsk);
extern void exit_rcu(struct task_struct *tsk);
extern void exit_task_stack(struct task_struct *tsk);
extern void exit_task_work(struct task_struct *tsk);
extern void exit_task_io(struct task_struct *tsk);
extern void exit_task_net(struct task_struct *tsk);
extern void exit_task_timer(struct task_struct *tsk);
extern void exit_task_vm(struct task_struct *tsk);
extern void exit_task_wq(struct task_struct *tsk);
extern void exit_task_xa(struct task_struct *tsk);

extern void wake_up_process(struct task_struct *p);

#define MAX_SCHEDULE_TIMEOUT ((long)(~0UL >> 1))



/*
 * Refcounted process identifier (linuxu/src/shims/task.c). Every linuxu
 * task leads its own thread group, so one struct pid serves PIDTYPE_PID
 * and PIDTYPE_TGID. The task holds one reference; @task is cleared when
 * the task is released, after which pid_task() returns NULL while
 * holders of other references keep a valid number.
 */
struct pid {
	refcount_t count;
	int nr;
	struct task_struct *task;
	struct list_head node;
};

/* Thread-group pid of @tsk, allocated on first use; no reference taken. */
struct pid *task_tgid(struct task_struct *tsk);

/* get_pid/pid_nr/put_pid/enum pid_type live in <linux/pid.h> */
#include <linux/pid.h>



/* task-state surface used by drm_syncobj wait paths */
#ifndef __set_current_state
#define __set_current_state(value) __atomic_store_n(&current->state, (value), __ATOMIC_RELEASE)
#endif
#ifndef set_current_state
#define set_current_state(state) __set_current_state(state)
#endif
#define in_task() 1
static inline bool signal_pending(struct task_struct *task)
{
	return task && __atomic_load_n(&task->pending_signals, __ATOMIC_ACQUIRE) != 0;
}

extern struct task_struct *get_task_struct(struct task_struct *tsk);
extern void put_task_struct(struct task_struct *tsk);

static inline void sched_set_fifo(struct task_struct *p) { (void)p; }
static inline void sched_set_fifo_realtime(struct task_struct *p) { (void)p; }
static inline void sched_set_normal(struct task_struct *p, int prio) { (void)p; (void)prio; }

#endif /* _LINUX_SCHED_H */
