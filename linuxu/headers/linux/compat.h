/* linuxu: SHIM (third_party/linux/include/linux/compat.h) */
#ifndef __LINUX_COMPAT_H
#define __LINUX_COMPAT_H

#include <linux/types.h>

struct file;
struct pt_regs;

/* linuxu: single-arch (no 32-bit compat) — compat tasks never exist */
#define is_compat_task() 0
static inline bool in_compat_syscall(void)
{
	return false;
}

#endif
