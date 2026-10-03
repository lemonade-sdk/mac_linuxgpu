/* linuxu: SHIM (third_party/linux/include/linux/nospec.h) */
#ifndef __LINUX_NOSPEC_H
#define __LINUX_NOSPEC_H
#include <linux/types.h>
#define array_index_nospec(idx, size) ((unsigned long)(idx) < (unsigned long)(size) ? (idx) : 0)
#endif
