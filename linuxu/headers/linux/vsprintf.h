/* linuxu: SHIM (third_party/linux/include/linux/vsprintf.h)
 * vsprintf surface used by the KMD: struct va_format.
 * (vscnprintf/vsnprintf etc. are declared in linuxu linux/printk.h)
 */
#ifndef _LINUX_VSPRINTF_H
#define _LINUX_VSPRINTF_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <stdarg.h>

struct va_format {
	char	*fmt;
	va_list	*va;
};



#endif /* _LINUX_VSPRINTF_H */
