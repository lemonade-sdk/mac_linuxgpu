/* linuxu: SHIM (third_party/linux/include/linux/errno.h + uapi/linux/errno.h)
 *
 * Linux errno numbers (not macOS's) via the asm/errno.h shadow.
 */
#ifndef __LINUX_ERRNO_H
#define __LINUX_ERRNO_H

#include <asm/errno.h>

/* Linux-ABI aliases the driver uses */
#define EPROBE_DEFER		517	/* Driver requests probe retry */

#endif /* __LINUX_ERRNO_H */

/* linuxu: not in macOS libc errno.h */
#ifndef ENOTSUPP
#define ENOTSUPP 62

#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#endif
