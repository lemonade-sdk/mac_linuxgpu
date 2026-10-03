/* linuxu: SHIM (third_party/linux/include/linux/printk.h)
 *
 * printk family -> linuxu/src/shims/printk.c (writes to stderr/log).
 */
#ifndef __LINUX_PRINTK_H
#define __LINUX_PRINTK_H

#ifndef asmlinkage
#define asmlinkage
#endif

#include <linux/stddef.h>
#include <stdarg.h>
#include <linux/kern_levels.h>
  #include <linux/compiler.h>
#include <asm/pt_regs.h>

#define BUILDTIME_PRINTK
#define NO_printk(fmt, ...)		do { } while (0)
#define NO_printk_ratelimited(fmt, ...)	do { } while (0)
#define NO_printk_once(fmt, ...)		do { } while (0)

#define pr_emerg(fmt, ...) printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)   printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warn_once(fmt, ...) printk_once(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_info_once(fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_warning_once(fmt, ...)  printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_err_once(fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_cont(fmt, ...)   printk(KERN_CONT fmt, ##__VA_ARGS__)
#define pr_fatal(fmt, ...)  printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_emerg_ratelimited(fmt, ...)  printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert_ratelimited(fmt, ...)  printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit_ratelimited(fmt, ...)   printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warning_ratelimited(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...)   printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice_ratelimited(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug_ratelimited(fmt, ...)  dynamic_pr_debug(fmt, ##__VA_ARGS__)


#define pr_debug(fmt, ...)	dynamic_pr_debug(fmt, ##__VA_ARGS__)
#define HW_ERR "[Hardware Error]: "
#define pr_devel(fmt, ...)	dynamic_pr_debug(fmt, ##__VA_ARGS__)

extern asmlinkage int vprintk(const char *fmt, va_list args);
struct dev_printk_info;
extern asmlinkage int vprintk_emit(int facility, int level,
	const struct dev_printk_info *info, const char *fmt, va_list args);
extern int printk(const char *fmt, ...) __printf(1, 2);
extern int dynamic_pr_debug(const char *fmt, ...) __printf(1, 2);
extern int printk_ratelimited(const char *fmt, ...) __printf(1, 2);
extern int printk_once(const char *fmt, ...) __printf(1, 2);
extern int printk_deferred(const char *fmt, ...) __printf(1, 2);
extern int vprintk_deferred(const char *fmt, va_list args);
extern int vprintk_sprint(const char *fmt, va_list args,
			  char *buf, size_t size);
extern int vscnprintf(char *buf, size_t size, const char *fmt,
		      va_list args);


#define pr_WARNING_once(fmt, ...)  printk(KERN_WARNING fmt, ##__VA_ARGS__)

static inline bool printk_ratelimit(void)
{
	return true;
}

#define print_hex_dump_debug(prefix_str, prefix_type, rowsize, \
			     groupsize, buf, len, ascii)		\
	print_hex_dump(KERN_DEBUG, prefix_str, prefix_type, rowsize,	\
		       groupsize, buf, len, ascii)


#define dev_err_once(dev, fmt, ...) dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, cond, fmt, ...) do { } while (0)
#define dev_WARN(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)

/* upstream linux/printk.h: set during oops/panic/BUG — no-ops in the shim */
#ifndef oops_in_progress
extern int oops_in_progress;
#define oops_in_progress 0
#endif

/* lib/hexdump.c (implemented in linuxu/src/shims/seq_file.c). */
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize,
		       char *linebuf, size_t linebuflen, bool ascii);

#endif /* __LINUX_PRINTK_H */
