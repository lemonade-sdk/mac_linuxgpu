/* linuxu: SHIM (third_party/linux/include/linux/sched/mm.h)
 *
 * What the KMD actually needs from this header:
 *   struct mm_struct * (opaque pointer type)
 *   current->mm / task->mm access patterns
 *
 * The real mm_struct is in linuxu/headers/linux/mm.h.
 * This header just makes sure mm_struct is visible when sched/mm.h
 * is included directly (it is included by xarray.h and others).
 */
#ifndef _LINUX_SCHED_MM_H
#define _LINUX_SCHED_MM_H

#include <linux/mm.h>   /* struct mm_struct */
#include <linux/pid.h>   /* struct pid, put_pid (header-only) */
#include <linux/sched.h> /* struct task_struct (complete) */

struct task_struct;

/* ---- pid namespace / pid helpers (kfd_chardev.c / kfd_smi_events.c) ---- */
struct pid_namespace;

/* find_get_pid/get_pid/put_pid/get_pid_task are owned by linux/pid.h */

static inline int task_pid_nr_ns(const struct task_struct *tsk,
				const struct pid_namespace *ns)
{
	(void)ns;
	return tsk->pid;
}

static inline int task_tgid_nr_ns(const struct task_struct *tsk,
				const struct pid_namespace *ns)
{
	(void)ns;
	return tsk->tgid;
}

static inline struct pid_namespace *task_active_pid_ns(
					const struct task_struct *tsk)
{
	(void)tsk;
	return NULL;
}

/* get_task_mm(), mmgrab/mmdrop and kthread_use_mm are declared in
 * <linux/mm.h> / <linux/kernel.h> and defined in linuxu/src/mm/mm.c. */


/* ---- memalloc scope (shim: no-op, returns a token) ---- */
static inline unsigned int memalloc_noreclaim_save(void)
{
	return 0;
}
static inline void memalloc_noreclaim_restore(unsigned int prev)
{
}

#endif /* _LINUX_SCHED_MM_H */
