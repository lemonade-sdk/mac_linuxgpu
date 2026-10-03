/* linuxu: SHIM (third_party/linux/include/linux/dynamic_debug.h) — no-op */
#ifndef _LINUX_DYNAMIC_DEBUG_H
#define _LINUX_DYNAMIC_DEBUG_H

#include <linux/printk.h>
#include <linux/stdarg.h>
#include <linux/types.h>

struct _ddebug {
	int descriptor;
	const char *module_name;
	const char *function;
	const char *format;
	const char *file;
	unsigned int line;
	unsigned int flags;
	unsigned long view_mask;
};

struct _dprintk_descriptors {
	const char *mod_name;
	int mod_idx;
	int descriptor;
	int count;
	struct _ddebug *descriptor_ptr;
};

struct ddebug_class {
	int ddebug_class_id;
	int loglevel;
	unsigned int flags;
};

#define DPRINTK_CLASS_ID_INVALID	-1
#define DPRINTK_LIB_MOD_IDX		0
#define DYNAMIC_DEBUG_NO_FLAGS		0
#define DYNAMIC_DEBUG_PR_FLAGS		1
#define DYNAMIC_DEBUG_TRACE_FLAGS	2
#define DYNAMIC_DEBUG_TRACE_BIT		0
#define DYNAMIC_DEBUG_TRACE_VAL		1
#define DYNAMIC_DEBUG_TRACE_FLAGS_MASK	(1U << DYNAMIC_DEBUG_TRACE_BIT)

#define DEFINE_DYNAMIC_DEBUG_METADATA(name, format) \
	struct _ddebug name = { \
		.format = (format), \
		.module_name = KBUILD_MODNAME, \
		.function = __func__, \
		.file = __FILE__, \
		.line = __LINE__, \
	}

#define DEFINE_DYNAMIC_DEBUG_METADATA_FLAGS(name, format, flags) \
	struct _ddebug name = { \
		.format = (format), \
		.module_name = KBUILD_MODNAME, \
		.function = __func__, \
		.file = __FILE__, \
		.line = __LINE__, \
		.flags = (flags), \
	}

#define dynamic_printk(fmt, ...) do { } while (0)
#define _dynamic_printk(fmt, ...) do { } while (0)
#define _dynamic_funcname_printk(fmt, ...) do { } while (0)
#define _dynamic_printk_flags(fmt, ...) do { } while (0)
#define _dynamic_funcname_printk_flags(fmt, ...) do { } while (0)
#define _dynamic_printk_once(fmt, ...) do { } while (0)
#define dynamic_printk_flags(fmt, ...) do { } while (0)
#define dynamic_printk_once(fmt, ...) do { } while (0)
#define dynamic_pr_debug(fmt, ...) do { } while (0)
#define dynamic_dev_dbg(dev, fmt, ...) do { } while (0)
#define dynamic_dev_err(dev, fmt, ...) do { } while (0)
#define dynamic_dev_info(dev, fmt, ...) do { } while (0)
#define dynamic_dev_notice(dev, fmt, ...) do { } while (0)
#define dynamic_dev_warn(dev, fmt, ...) do { } while (0)

extern int dynamic_debug_enabled(void);
extern int ddebug_register(const char *modname, struct _ddebug *db, int num);
extern int ddebug_remove_module(const char *modname);
extern int ddebug_exec(const char *cmd);
extern int ddebug_exec_query(const char *query, int (*callback)(struct _ddebug *dp,
							     const struct ddebug_class *cls));
extern struct ddebug_class *ddebug_class_find(const char *class_name);
extern struct ddebug_class *ddebug_class_find_or_create(const char *class_name);
extern void ddebug_class_delete(struct ddebug_class *cls);
extern struct ddebug_class *ddebug_class_ref(struct ddebug_class *cls);
extern void ddebug_class_unref(struct ddebug_class *cls);
extern void ddebug_dump_all(void);
extern void ddebug_module_init(const char *modname, const struct _ddebug *db, int num);
extern void ddebug_module_exit(const char *modname);

#endif /* _LINUX_DYNAMIC_DEBUG_H */
