/* linuxu: SHIM (third_party/linux/include/linux/moduleparam.h)
 * Userspace build has no kernel module parameters: the macros expand to
 * nothing (or a no-op static for _cb forms that need a symbol).
 */
#ifndef __LINUX_MODULEPARAM_H
#define __LINUX_MODULEPARAM_H

#include <linux/types.h>

#define module_param(name, type, perm)
#define module_param_named(name, val, type, perm)
#define module_param_string(name, val, perm)
#define module_param_int(name, val, perm)
#define module_param_long(name, val, perm)
#define module_param_short(name, val, perm)
#define module_param_byte(name, val, perm)
#define module_param_bool(name, val, perm)
#define module_param_cb(name, op, private, perm)
#define module_param_array(name, type, nump, perm)
#define module_param_call(name, set, get, perm)
#define __module_param_call(name, arg, set, get, perm, len, plus)
#define module_param_uninit(name, arg)
#define module_param_uninit_array(name, arg)
#define module_param_unsafe(name, type, perm)
#define module_param_optional(name, type, perm)

#endif /* __LINUX_MODULEPARAM_H */
