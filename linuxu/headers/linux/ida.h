/* linuxu: SHIM (third_party/linux/include/linux/ida.h) — the IDA (integer
 * allocator).  Upstream 6.x ida.h is a thin wrapper over the xarray
 * (struct ida = struct ida_alloc + xarray).  This shim keeps the
 * struct layout and the IDA_INIT macro; the runtime operations are
 * provided by linuxu/src. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IDA_H
#define _LINUX_IDA_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/xarray.h>

struct ida {
	struct xarray		ia_xa;
	unsigned int		ia_index;
	unsigned int		ia_free;
	unsigned int		ia_free_index;
};

#define IDA_INIT(name) {				\
	.ia_xa = { .xa_lock = __SPIN_LOCK_UNLOCKED(ia_xa.xa_lock), \
		   .xa_flags = 0, .xa_head = NULL },	\
	.ia_index = 0,				\
	.ia_free = 0,				\
	.ia_free_index = 0,			\
}

#define DEFINE_IDA(name)	struct ida name = IDA_INIT(name)

static inline void ida_init(struct ida *ida)
{
	*ida = (struct ida)IDA_INIT(ida);
}

void ida_destroy(struct ida *ida);
int ida_alloc_range(struct ida *ida, unsigned int lowest, unsigned int highest, gfp_t gfp);
int ida_alloc(struct ida *ida, gfp_t gfp);
int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp);
int ida_alloc_cyclic(struct ida *ida, gfp_t gfp);
int ida_alloc_range_cyclic(struct ida *ida, unsigned int lowest, unsigned int highest, gfp_t gfp);
void ida_free(struct ida *ida, unsigned int id);
void ida_free_continuous(struct ida *ida, unsigned int id1, unsigned int id2);
void ida_free_max(struct ida *ida, unsigned int max);
void ida_free_above(struct ida *ida, unsigned int above);
int ida_exists(struct ida *ida);
unsigned long ida_get_new_above(struct ida *ida, unsigned int hint, int *id);
unsigned long ida_get_new(struct ida *ida, int *id);
unsigned long ida_get_new_above_cyclic(struct ida *ida, unsigned int hint, int *id);
unsigned long ida_get_new_cyclic(struct ida *ida, int *id);
void ida_put(struct ida *ida, unsigned long id);
void ida_put_cyclic(struct ida *ida, unsigned long id);
int ida_available(struct ida *ida);


static inline int ida_alloc_min(struct ida *ida, unsigned int min, gfp_t gfp)
{
	return ida_alloc_range(ida, min, UINT_MAX, gfp);
}
#endif
