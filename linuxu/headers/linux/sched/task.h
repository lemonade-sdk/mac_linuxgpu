/* linuxu: SHIM (third_party/linux/include/linux/sched/task.h) */
#ifndef __LINUX_SCHED_TASK_H
#define __LINUX_SCHED_TASK_H

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/capability.h>

struct task_struct;

typedef int pid_t;
typedef int pid_t_;

#define PIDTYPE_PID	0
#define PIDTYPE_PGID	1
#define PIDTYPE_SID	2
#define PIDTYPE_TGID	3

struct pid;

/* NOTE: get_task_mm() is NOT defined here — the non-static definition
 * lives in linuxu/src/mm/usermem.c. Declaring a static inline here would
 * collide with that symbol in every TU that includes <linux/sched.h>.
 */

#endif /* __LINUX_SCHED_TASK_H */
