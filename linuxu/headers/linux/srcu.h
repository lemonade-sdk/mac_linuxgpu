/* linuxu: SHIM (third_party/linux/include/linux/srcu.h) — per-domain SRCU. */
#ifndef __LINUX_SRCU_H
#define __LINUX_SRCU_H

#include <linux/rcupdate.h>

#define DEFINE_STATIC_SRCU(name) DEFINE_SRCU(name)

static inline void init_srcu_struct_static(struct srcu_struct *sp)
{
	srcu_init_struct(sp);
}

#endif
