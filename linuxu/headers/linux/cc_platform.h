/* linuxu: SHIM (third_party/linux/include/linux/cc_platform.h) */
#ifndef __LINUX_CC_PLATFORM_H
#define __LINUX_CC_PLATFORM_H

#include <linux/types.h>

enum cc_attr {
	CC_ATTR_GUEST_MEM_ENCRYPT = 0,
	CC_ATTR_MEM_ENCRYPT = 1,
};

/* shim: confidential-computing not active on the macOS host */
static inline bool cc_platform_has(enum cc_attr attr)
{
	(void)attr;
	return false;
}

#endif
