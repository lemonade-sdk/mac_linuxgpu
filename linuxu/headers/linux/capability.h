/* linuxu: SHIM (third_party/linux/include/linux/capability.h) — single-process
 * userspace; all capability checks succeed. */
#ifndef _LINUX_CAPABILITY_H
#define _LINUX_CAPABILITY_H

#include <linux/types.h>

enum {
	CAP_SYS_NICE = 23,
	CAP_CHOWN = 0,
	CAP_DAC_OVERRIDE = 1,
	CAP_FOWNER = 3,
	CAP_KILL = 5,
	CAP_SETGID = 6,
	CAP_SETUID = 7,
	CAP_SYS_ADMIN = 21,
	CAP_SYS_PTRACE = 19,
	CAP_CHECKPOINT_RESTORE = 40,
};

struct kernel_cap_struct {
	unsigned long cap[2];
};

static inline bool capable(int cap)
{
	(void)cap;
	return true;
}


#endif
