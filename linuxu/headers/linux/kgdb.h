/* linuxu: EDITED (third_party/linux/include/linux/kgdb.h) - kernel debugger
 * not usable in userspace; keep only the symbols driver code references
 * (dc_breakpoint() -> kgdb_breakpoint(), used by display os_types.h). */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KGDB_H_
#define _KGDB_H_

#include <linux/types.h>

/*
 * kgdb_breakpoint - compiled in breakpoint.
 * In the userspace shim this is a plain compile-time no-op.
 */
#define kgdb_breakpoint() do { } while (0)

#endif /* _KGDB_H_ */
