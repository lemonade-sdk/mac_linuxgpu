/* linuxu: SHIM (third_party/linux/include/linux/init.h) — explicit module
 * lifecycle entry points. The dext calls them in a controlled order; no
 * global constructors run driver initialization. */
#ifndef __LINUX_INIT_H
#define __LINUX_INIT_H

#include <linux/compiler.h>

#define __init
#define __initdata
#define __initcall(fn)
#define __initcall_start()
#define __initcall_end()
#define device_initcall(fn)
#define late_device_initcall(fn)
#define platform_initcall(fn)
#define core_initcall(fn)
#define core_initcall_sync(fn)
#define postcore_initcall(fn)
#define arch_initcall(fn)
#define subsys_initcall(fn)
#define subsys_initcall_sync(fn)
#define fs_initcall(fn)
#define fs_initcall_sync(fn)
#define rootfs_initcall(fn)
#define console_initcall(fn)
#define console_initcall_sync(fn)
#define device_initcall_sync(fn)
#define device_initcall_sync_id(fn, id)
#undef module_init
#undef module_exit
#define module_init(fn) int linuxu_module_init_##fn(void) { return fn(); }
#define module_exit(fn) void linuxu_module_exit_##fn(void) { fn(); }
#define __exit
#define __exitdata

#define __initcall_meta(fn)

#define initcall_entry
#define __initcall_priority(prio)

#define __meminit
#define __meminitdata
#define __meminitconst

#endif /* __LINUX_INIT_H */
