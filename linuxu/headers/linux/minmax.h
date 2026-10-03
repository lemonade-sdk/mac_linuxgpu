/* linuxu: SHIM (third_party/linux/include/linux/minmax.h) - min()/max()/
 * clamp() and friends live in the shim linux/bitops.h, which every kernel
 * header path already includes; this name only forwards to it. */
#ifndef _LINUX_MINMAX_H
#define _LINUX_MINMAX_H
#include <linux/bitops.h>
#endif
