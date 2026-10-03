/* linuxu: SHIM (third_party/linux/include/linux/pid.h)
 *
 * Refcounted pid API backed by linuxu/src/shims/task.c. There are no pid
 * namespaces: virtual and global numbers are the task's tgid.
 */
#ifndef _LINUX_PID_H
#define _LINUX_PID_H

#include <linux/types.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

struct task_struct;
struct pid_namespace;



extern struct pid *get_task_pid(struct task_struct *tsk, int type);
extern struct pid *get_pid(struct pid *pid);
extern void put_pid(struct pid *pid);
extern struct task_struct *pid_task(struct pid *pid, int type);
extern struct task_struct *get_pid_task(struct pid *pid, int type);
extern struct pid *find_get_pid(pid_t nr);

/* Linux returns 0 for a NULL pid. */
extern pid_t pid_vnr(struct pid *pid);
static inline pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	(void)ns;
	return pid_vnr(pid);
}

#define pid_nr(pid) pid_nr_ns((pid), (struct pid_namespace *)NULL)

/* enum pid_type (vendor 2026 pid.h); PIDTYPE_PID must stay value 0 as in
 * upstream. */
enum pid_type {
	PIDTYPE_PID = 0,
	PIDTYPE_PGID,
	PIDTYPE_SID,
	PIDTYPE_TGID,
	PIDTYPE_MAX,
};

extern int task_pid_vnr(const struct task_struct *task);


#endif /* _LINUX_PID_H */
