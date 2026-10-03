/* linuxu: SHIM (third_party/linux/include/linux/ptrace.h) */
#ifndef __LINUX_PTRACE_H
#define __LINUX_PTRACE_H

#include <linux/sched.h>

static inline struct task_struct *ptrace_parent(struct task_struct *child)
{
	(void)child;
	return NULL;
}

#endif
