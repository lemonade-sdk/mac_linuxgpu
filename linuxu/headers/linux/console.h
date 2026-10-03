/* linuxu: SHIM (third_party/linux/include/linux/console.h) */
#ifndef __LINUX_CONSOLE_H
#define __LINUX_CONSOLE_H

#include <linux/types.h>   /* bool */

static inline void console_lock(void) {}
static inline void console_unlock(void) {}
static inline bool console_trylock(void) { return true; }
#endif
