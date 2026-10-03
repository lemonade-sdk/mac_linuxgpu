/* linuxu: SHIM (third_party/linux/include/linux/mm_types.h)
 *
 * The pinned vendor tree splits mm types into mm_types.h (struct
 * mm_struct / vm_area_struct / task_struct); the host shim keeps them
 * in <linux/mm.h> / <linux/sched.h>. This header just pulls those in
 * so `#include <linux/mm_types.h>` sites compile unmodified.
 */
#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/mm.h>
#include <linux/sched.h>

#endif /* _LINUX_MM_TYPES_H */
