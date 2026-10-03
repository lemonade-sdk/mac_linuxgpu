/* linuxu: EDITED (third_party/linux/include/drm/drm_print.h) — the
 * drm_printer / DRM_ERROR / drm_info / drm_warn / drm_err / dev_*
 * logging surface.  Kernel-only includes (printk.h, device.h,
 * dynamic_debug.h) swapped for linuxu shadows; DRM_DEV_*_RATELIMITED
 * (which needs the rate-limit machinery) reduced to plain DRM_DEV_*. */
#ifndef DRM_PRINT_H_
#define DRM_PRINT_H_

#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/vsprintf.h>

#ifndef _THIS_IP_
#define _THIS_IP_		((unsigned long)0)
#endif
#include <linux/dynamic_debug.h>
#include <linux/vsprintf.h>

#include <drm/drm.h>
#include <drm/drm_device.h>

struct debugfs_regset32;
struct drm_device;
struct seq_file;

/* Do *not* use outside of drm_print.[ch]! */
extern unsigned long __drm_debug;

/**
 * enum drm_debug_category - The DRM debug categories
 */
enum drm_debug_category {
	DRM_UT_CORE,
	DRM_UT_DRIVER,
	DRM_UT_KMS,
	DRM_UT_PRIME,
	DRM_UT_ATOMIC,
	DRM_UT_VBL,
	DRM_UT_STATE,
	DRM_UT_LEASE,
	DRM_UT_DP,
	DRM_UT_DRMRES
};

static inline bool drm_debug_enabled_raw(enum drm_debug_category category)
{
	return unlikely(__drm_debug & BIT(category));
}

#define drm_debug_enabled_instrumented(category)			\
	({								\
		pr_debug("todo: is this frequent enough to optimize ?\n"); \
		drm_debug_enabled_raw(category);			\
	})

#if defined(CONFIG_DRM_USE_DYNAMIC_DEBUG)
#define __drm_debug_enabled(category)	true
#define drm_debug_enabled(category)	drm_debug_enabled_instrumented(category)
#else
#define __drm_debug_enabled(category)	drm_debug_enabled_raw(category)
#define drm_debug_enabled(category)	drm_debug_enabled_raw(category)
#endif

/**
 * struct drm_printer - drm output "stream"
 */
struct drm_printer {
	/* private: */
	void (*printfn)(struct drm_printer *p, struct va_format *vaf);
	void (*puts)(struct drm_printer *p, const char *str);
	void *arg;
	const void *origin;
	const char *prefix;
	struct {
		unsigned int series;
		unsigned int counter;
	} line;
	enum drm_debug_category category;
};

void __drm_printfn_coredump(struct drm_printer *p, struct va_format *vaf);
void __drm_puts_coredump(struct drm_printer *p, const char *str);
void __drm_printfn_seq_file(struct drm_printer *p, struct va_format *vaf);
void __drm_puts_seq_file(struct drm_printer *p, const char *str);
void __drm_printfn_info(struct drm_printer *p, struct va_format *vaf);
void __drm_printfn_dbg(struct drm_printer *p, struct va_format *vaf);
void __drm_printfn_err(struct drm_printer *p, struct va_format *vaf);
void __drm_printfn_line(struct drm_printer *p, struct va_format *vaf);

__printf(2, 3)
void drm_printf(struct drm_printer *p, const char *f, ...);
void drm_puts(struct drm_printer *p, const char *str);
void drm_print_regset32(struct drm_printer *p, struct debugfs_regset32 *regset);
void drm_print_bits(struct drm_printer *p, unsigned long value,
		    const char * const bits[], unsigned int nbits);
void drm_print_hex_dump(struct drm_printer *p, const char *prefix,
			const u8 *buf, size_t len);

__printf(2, 0)
static inline void
drm_vprintf(struct drm_printer *p, const char *fmt, va_list *va)
{
	struct va_format vaf = { .fmt = fmt, .va = va };

	p->printfn(p, &vaf);
}

/**
 * drm_printf_indent - Print to a &drm_printer stream with indentation
 */
#define drm_printf_indent(printer, indent, fmt, ...) \
	drm_printf((printer), "%.*s" fmt, (indent), "\t\t\t\t\tX", ##__VA_ARGS__)

/**
 * struct drm_print_iterator - local struct used with drm_printer_coredump
 */
struct drm_print_iterator {
	void *data;
	ssize_t start;
	ssize_t remain;
	/* private: */
	ssize_t offset;
};

/**
 * drm_coredump_printer - construct a &drm_printer that can output to a buffer
 */
static inline struct drm_printer
drm_coredump_printer(struct drm_print_iterator *iter)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_coredump,
		.puts = __drm_puts_coredump,
		.arg = iter,
	};

	/* Set the internal offset of the iterator to zero */
	iter->offset = 0;

	return p;
}

/**
 * drm_coredump_printer_is_full() - DRM coredump printer output is full
 */
static inline bool drm_coredump_printer_is_full(struct drm_printer *p)
{
	struct drm_print_iterator *iterator = p->arg;

	if (p->printfn != __drm_printfn_coredump)
		return true;

	return !iterator->remain;
}

/**
 * drm_seq_file_printer - construct a &drm_printer that outputs to &seq_file
 */
static inline struct drm_printer drm_seq_file_printer(struct seq_file *f)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_seq_file,
		.puts = __drm_puts_seq_file,
		.arg = f,
	};
	return p;
}

/**
 * drm_info_printer - construct a &drm_printer that outputs to dev_printk()
 */
static inline struct drm_printer drm_info_printer(struct device *dev)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_info,
		.arg = dev,
	};
	return p;
}

/**
 * drm_dbg_printer - construct a &drm_printer for drm device specific output
 */
static inline struct drm_printer drm_dbg_printer(struct drm_device *drm,
						 enum drm_debug_category category,
						 const char *prefix)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_dbg,
		.arg = drm,
		.origin = (const void *)_THIS_IP_, /* it's fine as we will be inlined */
		.prefix = prefix,
		.category = category,
	};
	return p;
}

/**
 * drm_err_printer - construct a &drm_printer that outputs to drm_err()
 */
static inline struct drm_printer drm_err_printer(struct drm_device *drm,
						 const char *prefix)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_err,
		.arg = drm,
		.prefix = prefix
	};
	return p;
}

/**
 * drm_line_printer - construct a &drm_printer that prefixes outputs with line numbers
 */
static inline struct drm_printer drm_line_printer(struct drm_printer *p,
						  const char *prefix,
						  unsigned int series)
{
	struct drm_printer lp = {
		.printfn = __drm_printfn_line,
		.arg = p,
		.prefix = prefix,
		.line = { .series = series, },
	};
	return lp;
}

/*
 * struct device based logging
 */

__printf(3, 4)
void drm_dev_printk(const struct device *dev, const char *level,
		    const char *format, ...);
struct _ddebug;
__printf(4, 5)
void __drm_dev_dbg(struct _ddebug *desc, const struct device *dev,
		   enum drm_debug_category category, const char *format, ...);

/**
 * DRM_DEV_ERROR() - Error output.
 */
#define DRM_DEV_ERROR(dev, fmt, ...)					\
	drm_dev_printk(dev, KERN_ERR, "*ERROR* " fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_info() or dev_info(). */
#define DRM_DEV_INFO(dev, fmt, ...)				\
	drm_dev_printk(dev, KERN_INFO, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_info_once() or dev_info_once(). */
#define DRM_DEV_INFO_ONCE(dev, fmt, ...)				\
({									\
	static bool __print_once __read_mostly;				\
	if (!__print_once) {						\
		__print_once = true;					\
		DRM_DEV_INFO(dev, fmt, ##__VA_ARGS__);			\
	}								\
})

/*
 * EDITED: upstream DRM_DEV_ERROR_RATELIMITED embeds
 * DEFINE_RATELIMIT_STATE + __ratelimit(); the linuxu shadow
 * linux/printk.h does not carry the rate-limit machinery, so the
 * rate-limited variant falls through to the plain form (the compute
 * build's log volume does not depend on rate limiting).
 */
#define DRM_DEV_ERROR_RATELIMITED(dev, fmt, ...) \
	DRM_DEV_ERROR(dev, fmt, ##__VA_ARGS__)

#if !defined(CONFIG_DRM_USE_DYNAMIC_DEBUG)
#define drm_dev_dbg(dev, cat, fmt, ...)				\
	__drm_dev_dbg(NULL, dev, cat, fmt, ##__VA_ARGS__)
#else
#define drm_dev_dbg(dev, cat, fmt, ...)				\
	_dynamic_func_call_cls(cat, fmt, __drm_dev_dbg,		\
			       dev, cat, fmt, ##__VA_ARGS__)
#endif

/**
 * DRM_DEV_DEBUG() - Debug output for generic drm code
 */
#define DRM_DEV_DEBUG(dev, fmt, ...)					\
	drm_dev_dbg(dev, DRM_UT_CORE, fmt, ##__VA_ARGS__)
/**
 * DRM_DEV_DEBUG_DRIVER() - Debug output for vendor specific part
 */
#define DRM_DEV_DEBUG_DRIVER(dev, fmt, ...)				\
	drm_dev_dbg(dev, DRM_UT_DRIVER,	fmt, ##__VA_ARGS__)
/**
 * DRM_DEV_DEBUG_KMS() - Debug output for modesetting code
 */
#define DRM_DEV_DEBUG_KMS(dev, fmt, ...)				\
	drm_dev_dbg(dev, DRM_UT_KMS, fmt, ##__VA_ARGS__)

/*
 * struct drm_device based logging
 */

/* Helper to enforce struct drm_device type */
static inline struct device *__drm_to_dev(const struct drm_device *drm)
{
	return drm ? drm->dev : NULL;
}

/* Helper for struct drm_device based logging. */
#define __drm_printk(drm, level, type, fmt, ...)			\
	dev_##level##type(__drm_to_dev(drm), "[drm] " fmt, ##__VA_ARGS__)

#define drm_info(drm, fmt, ...)					\
	__drm_printk((drm), info,, fmt, ##__VA_ARGS__)

#define drm_notice(drm, fmt, ...)				\
	__drm_printk((drm), notice,, fmt, ##__VA_ARGS__)

#define drm_warn(drm, fmt, ...)					\
	__drm_printk((drm), warn,, fmt, ##__VA_ARGS__)

#define drm_err(drm, fmt, ...)					\
	__drm_printk((drm), err,, "*ERROR* " fmt, ##__VA_ARGS__)

#define drm_info_once(drm, fmt, ...)				\
	__drm_printk((drm), info, _once, fmt, ##__VA_ARGS__)

#define drm_notice_once(drm, fmt, ...)				\
	__drm_printk((drm), notice, _once, fmt, ##__VA_ARGS__)

#define drm_warn_once(drm, fmt, ...)				\
	__drm_printk((drm), warn, _once, fmt, ##__VA_ARGS__)

#define drm_err_once(drm, fmt, ...)				\
	__drm_printk((drm), err, _once, "*ERROR* " fmt, ##__VA_ARGS__)

#define drm_err_ratelimited(drm, fmt, ...)				\
	__drm_printk((drm), err, _ratelimited, "*ERROR* " fmt, ##__VA_ARGS__)

#define drm_dbg_core(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_CORE, fmt, ##__VA_ARGS__)
#define drm_dbg_driver(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DRIVER, fmt, ##__VA_ARGS__)
#define drm_dbg_kms(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_KMS, fmt, ##__VA_ARGS__)
#define drm_dbg_prime(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_PRIME, fmt, ##__VA_ARGS__)
#define drm_dbg_atomic(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_ATOMIC, fmt, ##__VA_ARGS__)
#define drm_dbg_vbl(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_VBL, fmt, ##__VA_ARGS__)
#define drm_dbg_state(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_STATE, fmt, ##__VA_ARGS__)
#define drm_dbg_lease(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_LEASE, fmt, ##__VA_ARGS__)
#define drm_dbg_dp(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DP, fmt, ##__VA_ARGS__)
#define drm_dbg_drmres(drm, fmt, ...)					\
	drm_dev_dbg(__drm_to_dev(drm), DRM_UT_DRMRES, fmt, ##__VA_ARGS__)

#define drm_dbg(drm, fmt, ...)	drm_dbg_driver(drm, fmt, ##__VA_ARGS__)

/*
 * printk based logging
 */

__printf(1, 2)
void __drm_err(const char *format, ...);

#if !defined(CONFIG_DRM_USE_DYNAMIC_DEBUG)
#define __drm_dbg(cat, fmt, ...)	__drm_dev_dbg(NULL, NULL, cat, fmt, ##__VA_ARGS__)
#else
#define __drm_dbg(cat, fmt, ...)					\
	_dynamic_func_call_cls(cat, fmt, __drm_dev_dbg,		\
			       NULL, cat, fmt, ##__VA_ARGS__)
#endif

/* Macros to make printk easier */

#define _DRM_PRINTK(once, level, fmt, ...)				\
	pr_info("[" DRM_NAME "] " fmt, ##__VA_ARGS__)
#define DRM_NAME "drm"

/* NOTE: this is deprecated in favor of pr_info(). */
#define DRM_INFO(fmt, ...)						\
	_DRM_PRINTK(, INFO, fmt, ##__VA_ARGS__)
/* NOTE: this is deprecated in favor of pr_notice(). */
#define DRM_NOTE(fmt, ...)						\
	_DRM_PRINTK(, NOTICE, fmt, ##__VA_ARGS__)
/* NOTE: this is deprecated in favor of pr_warn(). */
#define DRM_WARN(fmt, ...)						\
	_DRM_PRINTK(, WARNING, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of pr_info_once(). */
#define DRM_INFO_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, INFO, fmt, ##__VA_ARGS__)
/* NOTE: this is deprecated in favor of pr_notice_once(). */
#define DRM_NOTE_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, NOTICE, fmt, ##__VA_ARGS__)
/* NOTE: this is deprecated in favor of pr_warn_once(). */
#define DRM_WARN_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, WARNING, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of pr_err(). */
#define DRM_ERROR(fmt, ...)						\
	__drm_err(fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of pr_err_ratelimited(). */
#define DRM_ERROR_RATELIMITED(fmt, ...)					\
	DRM_DEV_ERROR_RATELIMITED(NULL, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_core(NULL, ...). */
#define DRM_DEBUG(fmt, ...)						\
	__drm_dbg(DRM_UT_CORE, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg(NULL, ...). */
#define DRM_DEBUG_DRIVER(fmt, ...)					\
	__drm_dbg(DRM_UT_DRIVER, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_kms(NULL, ...). */
#define DRM_DEBUG_KMS(fmt, ...)						\
	__drm_dbg(DRM_UT_KMS, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_prime(NULL, ...). */
#define DRM_DEBUG_PRIME(fmt, ...)					\
	__drm_dbg(DRM_UT_PRIME, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_atomic(NULL, ...). */
#define DRM_DEBUG_ATOMIC(fmt, ...)					\
	__drm_dbg(DRM_UT_ATOMIC, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_vbl(NULL, ...). */
#define DRM_DEBUG_VBL(fmt, ...)					\
	__drm_dbg(DRM_UT_VBL, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_lease(NULL, ...). */
#define DRM_DEBUG_LEASE(fmt, ...)					\
	__drm_dbg(DRM_UT_LEASE, fmt, ##__VA_ARGS__)

/* NOTE: this is deprecated in favor of drm_dbg_dp(NULL, ...). */
#define DRM_DEBUG_DP(fmt, ...)						\
	__drm_dbg(DRM_UT_DP, fmt, ## __VA_ARGS__)

#define __DRM_DEFINE_DBG_RATELIMITED(category, drm, fmt, ...)				\
({										\
	if (drm_debug_enabled(DRM_UT_ ## category))				\
		drm_dev_printk(__drm_to_dev(drm), KERN_DEBUG, fmt, ## __VA_ARGS__);	\
})

#define drm_dbg_ratelimited(drm, fmt, ...) \
	__DRM_DEFINE_DBG_RATELIMITED(DRIVER, drm, fmt, ##__VA_ARGS__)

#define drm_dbg_kms_ratelimited(drm, fmt, ...) \
	__DRM_DEFINE_DBG_RATELIMITED(KMS, drm, fmt, ##__VA_ARGS__)

/*
 * struct drm_device based WARNs
 */

#define drm_WARN(drm, condition, format, arg...)			\
	WARN(condition, "%s %s: [drm] " format,			\
			dev_driver_string(__drm_to_dev(drm)),		\
			dev_name(__drm_to_dev(drm)), ## arg)

#define drm_WARN_ONCE(drm, condition, format, arg...)			\
	WARN_ONCE(condition, "%s %s: [drm] " format,			\
			dev_driver_string(__drm_to_dev(drm)),		\
			dev_name(__drm_to_dev(drm)), ## arg)

#define drm_WARN_ON(drm, x)						\
	drm_WARN((drm), (x), "%s",					\
		 "drm_WARN_ON(" __stringify(x) ")")

#define drm_WARN_ON_ONCE(drm, x)					\
	drm_WARN_ONCE((drm), (x), "%s",				\
		      "drm_WARN_ON_ONCE(" __stringify(x) ")")

#endif /* DRM_PRINT_H_ */
