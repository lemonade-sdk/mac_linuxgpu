/* linuxu: SHIM (third_party/linux/include/linux/signal.h)
 * The driver includes <linux/sched/signal.h> (2026 split); on the host the
 * POSIX <signal.h> set already provides sigset_t/sigemptyset/sigaddset/
 * sigismember, so this kernel header only needs to exist and stay clean. */
#ifndef _LINUX_SIGNAL_H
#define _LINUX_SIGNAL_H

#include <signal.h>

struct task_struct;

/* Signals are delivered to shim tasks and interrupt their Linux waits.
 * They are never forwarded to host processes or used to kill DriverKit. */
extern int send_sig(int sig, struct task_struct *task, int privileged);
extern int send_signal(int sig, struct task_struct *task);

#endif /* _LINUX_SIGNAL_H */
