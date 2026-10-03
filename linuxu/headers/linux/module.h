/* linuxu: SHIM (third_party/linux/include/linux/module.h) - userspace dext */
#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

#include <linux/types.h>
#include <linux/init.h>

struct module {
	const char *name;
	const char *license;
};


#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_ALIAS(x)
#define MODULE_SOFTDEP(x)
#define MODULE_VERSION(x)
#define MODULE_INFO(tag, info)
#define MODULE_IMPORT_NS(ns)	MODULE_INFO(import_ns, ns)

/* module_param_named / module_param — no-op in userspace */
#define module_param_named(name, var, type, perm)
#define module_param(name, type, perm)
#define module_param_named_unsafe(name, var, type, perm)
#define module_param_unsafe(name, type, perm)
#define MODULE_PARM_DESC(name, desc)

/* DECLARE_DYNDBG_CLASSMAP — no-op */
#define DECLARE_DYNDBG_CLASSMAP(name, type, base, ...) 	static const int *name[] = { NULL }
#define DD_CLASS_TYPE_DISJOINT_BITS 0

/* Single-link userspace: no symbol export tables. All export macros no-op. */
#ifndef EXPORT_SYMBOL
#define EXPORT_SYMBOL(x)
#endif
#ifndef EXPORT_SYMBOL_GPL
#define EXPORT_SYMBOL_GPL(x)
#endif
#ifndef EXPORT_SYMBOL_FOR_TESTS_ONLY
#define EXPORT_SYMBOL_FOR_TESTS_ONLY(x)
#endif
#ifndef ALLOW_ERROR_INJECTION
#define ALLOW_ERROR_INJECTION(sym, type)
#endif

extern void module_get(struct module *m);
extern void module_put(struct module *m);
static inline bool try_module_get(struct module *m)
{
	if (!m)
		return true; /* built-in file operations have no module owner */
	module_get(m);
	return true;
}
static inline bool module_reference_locked(const struct module *m)
{
	return true;
}
#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "amdgpu"
#endif

static struct module __linuxu_this_module;
#ifndef THIS_MODULE
#define THIS_MODULE (&__linuxu_this_module)
#endif

#endif /* _LINUX_MODULE_H */
