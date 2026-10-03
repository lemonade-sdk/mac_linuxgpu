#ifndef LINUXU_RT_TASK_H
#define LINUXU_RT_TASK_H

#include <linux/sched.h>

void linuxu_task_init(struct task_struct *task, const char *name,
		      unsigned int flags);
int linuxu_task_swap_current(struct task_struct *task,
			    struct task_struct **previous);
void linuxu_task_wake(struct task_struct *task);
/* The calling thread's task if one exists, without creating the default
 * per-thread task (NULL then): for paths that only need to observe its
 * mm, files or signals. */
struct task_struct *linuxu_current_task_peek(void);
void linuxu_task_cleanup_current(void);
bool linuxu_task_fatal_signal_pending(struct task_struct *task);
void linuxu_task_clear_signals(struct task_struct *task);

#endif
