/* linuxu: SHIM (third_party/linux/include/linux/bug.h)
 *
 * BUG()/WARN family for the userspace shim.
 */
#ifndef __LINUX_BUG_H
#define __LINUX_BUG_H

#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/printk.h>

/* Runtime-provided (linuxu/src/bug.c). Never returns: the dext parks the
 * thread after quarantining the device, host builds abort. */
extern void linuxu_bug(const char *file, int line) __attribute__((noreturn));
extern void linuxu_warn(const char *file, int line, const char *fmt, ...)
	__printf(3, 4);

#define do_not_care(x) (void)(x)

#define BUG() \
	do { linuxu_bug(__FILE__, __LINE__); unreachable(); } while (0)

#define BUG_ON(condition) \
	do { if (unlikely(condition)) linuxu_bug(__FILE__, __LINE__); } while (0)
#define BUG_ON_MSG(condition, msg) BUG_ON(condition)

#define WARN(condition, format, ...) \
	({									\
		static int __warned;						\
		int __ret_warn = !!(condition);					\
		if (unlikely(__ret_warn))						\
			linuxu_warn(__FILE__, __LINE__, "WARN: " format, ##__VA_ARGS__); \
		__ret_warn;							\
	})
#define WARN_ON(condition) \
	WARN(condition, "WARNING: %s:%d\n", __FILE__, __LINE__)
#define WARN_ON_ONCE(condition) \
	({ \
		static int __warned; \
		int __ret_warn = !!(condition); \
		if (unlikely(__ret_warn && !__warned)) { \
			__warned = 1; \
			linuxu_warn(__FILE__, __LINE__, "WARNING: %s:%d\n", __FILE__, __LINE__); \
		} \
		__ret_warn; \
	})

#define WARN_ONCE(condition, format, ...) \
	({ \
		static int __warned; \
		int __ret_warn = !!(condition); \
		if (unlikely(__ret_warn && !__warned)) { \
			__warned = 1; \
			linuxu_warn(__FILE__, __LINE__, "WARN: " format, ##__VA_ARGS__); \
		} \
		__ret_warn; \
	})
#define BUG_DETERMINISTIC()	BUG()

#endif /* __LINUX_BUG_H */
