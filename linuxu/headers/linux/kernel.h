/* linuxu: SHIM (third_party/linux/include/linux/kernel.h)
 *
 * Core miscellany: IS_ENABLED, ALIGN, power-of-two helpers, ilog2,
 * stringify. printk family comes from linux/printk.h.
 */
#ifndef __LINUX_KERNEL_H
#define __LINUX_KERNEL_H

#include <asm/pt_regs.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#ifndef DIV_ROUND_CLOSEST
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#endif

#include <linux/types.h>
#include <linux/security.h>
#include <linux/ratelimit.h>
#include <linux/jiffies.h>

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif
#include <linux/stddef.h>
#include <linux/compiler.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/limits.h>
#include <linux/build_bug.h>
#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/overflow.h>
#include <linux/math64.h>
#include <asm/byteorder.h>	/* upstream kernel.h includes it too */
/* Upstream reaches str_*() through deep module.h/seq_file.h chains. */
#include <linux/string_choices.h>


#include <stdarg.h>

/* Config introspection is defined once in linux/list.h from autoconf.h. */
#include <linux/list.h>

#define MODULE_ALIAS(x)
#define MODULE_INFO(tag, info)
#define MODULE_SOFTDEP(x)
#define MODULE_VERMAGIC(x)
#ifndef module_init
#define module_init(fn)
#endif
#ifndef module_exit
#define module_exit(fn)
#endif
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_PARM_DESC(name, desc)
#define module_param(name, type, perm)
#define module_param_named(name, val, type, perm)
#define module_param_string(name, str, len, perm)
#define module_param_cb(name, op, p, perm)
#define module_param_array(name, type, n, perm)

#define __stringify(a)		# a
#define stringify(a)		__stringify(a)

/* ---- type helpers ---- */
#define typecheck(type, x)	((typeof(x) *)0)
#define __typecheck(type, x)	__builtin_types_compatible_p(typeof(x), type)

/* ---- alignment / rounding ---- */
#include <linux/align.h>

#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define roundup_pow_of_two(n) ((n) > 1 ? (1UL << (ilog2((unsigned long long)(n) - 1) + 1)) : 1UL)
#define rounddown_pow_of_two(n)	BIT(ilog2(n))

static __always_inline unsigned int ilog2(unsigned long long n)
{
	return n ? 63 - __builtin_clzll(n) : 0;
}
static __always_inline int ilog2_u32(unsigned int n)
{
	return n ? 31 - __builtin_clz(n) : 0;
}


/* ---- signed min/max (kernel.h names) ---- */
#define smin(a, b)	min(a, b)
#define smax(a, b)	max(a, b)
#define smin_t(type, a, b)	min_t(type, a, b)
#define smax_t(type, a, b)	max_t(type, a, b)
#define min3(a, b, c) min(min(a, b), c)
#define max3(a, b, c) max(max(a, b), c)
#define umin(a, b) min_t(unsigned long, a, b)
#define umax(a, b) max_t(unsigned long, a, b)

static inline unsigned long untagged_addr(unsigned long x)
{
	(void)x;
	return x;
}

#define min_not_zero(a, b) \
	({ typeof(a) _a = (a); typeof(b) _b = (b); \
	    (_a) == 0 ? (_b) : ((_b) == 0 ? (_a) : min(_a, _b)); })
#define max_not_zero(a, b) \
	({ typeof(a) _a = (a); typeof(b) _b = (b); \
	    (_a) == 0 ? (_b) : ((_b) == 0 ? (_a) : max(_a, _b)); })

#define div_s64(n, d)	((s64)(n) / (d))
#define __div_s64(n, d)	((s64)(n) / (d))

/* ---- panic / emergency (runtime: linuxu/src/shims/printk.c) ---- */
/* Never returns: the dext quarantines and parks the thread (rt/fatal.h). */
extern void panic(const char *fmt, ...) __printf(1, 2) __attribute__((noreturn));
extern void die(const char *str, struct pt_regs *regs, int err);
extern void dump_stack(void);

/* ---- alloc (runtime: linuxu/src/kmem/kmemalloc.c) ---- */
extern void *kcalloc(size_t n, size_t size, gfp_t flags);
extern void *kvmalloc(size_t size, gfp_t flags);
extern void *kvzalloc(size_t size, gfp_t flags);
extern void kvfree(const void *addr);
extern int scnprintf(char *buf, size_t size, const char *fmt, ...);
extern char *scnprintf_ptr(char *buf, size_t size, const void *ptr);

/* ---- page / user copy (runtime: linuxu/src/kmem/kmemalloc.c) ---- */
extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
extern void free_pages(unsigned long addr, unsigned int order);

/* linuxu: mm / kthread helpers used by the KFD path */
static inline void pagefault_disable(void) { }
static inline void pagefault_enable(void) { }
/* Adopt/drop a user mm on the current task (linuxu/src/mm/mm.c). */
struct mm_struct;
extern void kthread_use_mm(struct mm_struct *mm);
extern void kthread_unuse_mm(struct mm_struct *mm);
#include <linux/uaccess.h>
extern void mmgrab(struct mm_struct *mm);
extern void mmdrop(struct mm_struct *mm);
/* Reuse the embedded callback, avoiding allocation during deferred release. */
#define kfree_rcu(ptr, name) do { \
	__typeof__(ptr) __linuxu_rcu_object = (ptr); \
	if (__linuxu_rcu_object) { \
		extern void linuxu_kfree_rcu_callback(struct rcu_head *); \
		extern void call_rcu(struct rcu_head *, void (*)(struct rcu_head *)); \
		__linuxu_rcu_object->name.linuxu_free_pointer = __linuxu_rcu_object; \
		call_rcu(&__linuxu_rcu_object->name, linuxu_kfree_rcu_callback); \
	} \
} while (0)

#ifndef fallthrough
#define fallthrough do { } while (0)
#endif

#ifndef roundup
#define roundup(x, y)		((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y)		((x) - ((x) % (y)))
#endif

#include <linux/sizes.h>

static inline const char *str_read_write(bool is_read) { return is_read ? "read" : "write"; }


/* u64_to_user_ptr - cast a pointer passed as u64 from user space to void __user *
 * (third_party/linux/include/linux/util_macros.h). Must be a real void __user * so
 * callers can pass the result straight to copy_from_user/copy_to_user. */
static inline void __user *u64_to_user_ptr(const u64 x)
{
	return (void __user *)(unsigned long)x;
}

#ifndef swap
#define swap(x, y) \
	do { typeof(x) __tmp = (x); (x) = (y); (y) = __tmp; } while (0)
#endif

#ifndef orderly_poweroff
#define orderly_poweroff(force_it) do { } while (0)
#endif


#ifndef __PASTE
#define ___LINUXU_PASTE(a, b) a##b
#define __PASTE(a, b) ___LINUXU_PASTE(a, b)
#endif

#ifndef DUMP_PREFIX_NONE
#define DUMP_PREFIX_NONE 0
#define DUMP_PREFIX_ADDRESS 1
#define DUMP_PREFIX_OFFSET 2
#endif
#ifndef print_hex_dump
#define print_hex_dump(prefix, type, cols, len, buf, off, ...) do { } while (0)
#define print_hex_dump_bytes(prefix_str, prefix_type, buf, len) do { } while (0)
#endif
#ifndef print_hex_dump_bytes
#define print_hex_dump_bytes(len, type, buf) \
	do { (void)(len); (void)(type); (void)(buf); } while (0)
#endif

/* kernel_write (upstream linux/fs.h) — shim: no-op file write */
extern ssize_t kernel_write(struct file *file, const void *buf, size_t count,
			    loff_t *pos);

#endif /* __LINUX_KERNEL_H */
