/* linuxu: SHIM (third_party/linux/include/linux/idr.h) - IDR is a thin
 * wrapper over the xarray API upstream; the shim provides its own
 * struct idr layout (xarray_root, see linux/xarray.h from the shim
 * set) so idr.h stays API-compatible. Upstream semantics kept;
 * percpu/cleanup includes dropped. */
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * include/linux/idr.h
 *
 * Small id to pointer translation service avoiding fixed sized
 * tables.
 */

#ifndef __IDR_H__
#define __IDR_H__

#include <linux/gfp.h>
#include <linux/xarray.h>

struct idr {
	struct xarray		idr_rt;
	unsigned int		idr_base;
	unsigned int		idr_next;
};

/*
 * The IDR API does not expose the tagging functionality of the radix tree
 * to users.  Use tag 0 to track whether a node has free space below it.
 */
#define IDR_FREE	0

#define IDR_INIT_BASE(name, base) {					\
	.idr_rt = { .xa_lock = __SPIN_LOCK_UNLOCKED(idr_rt.xa_lock), \
		   .xa_flags = 0, .xa_head = NULL },			\
	.idr_base = (base),						\
	.idr_next = 0,							\
}

/**
 * IDR_INIT() - Initialise an IDR.
 * @name: Name of IDR.
 */
#define IDR_INIT(name)	IDR_INIT_BASE(name, 0)

/**
 * DEFINE_IDR() - Define a statically-allocated IDR.
 * @name: Name of the IDR.
 */
#define DEFINE_IDR(name)	struct idr name = IDR_INIT(name)

static inline void idr_init(struct idr *idr)
{
	*idr = (struct idr)IDR_INIT(idr);
}

static inline void idr_init_base(struct idr *idr, unsigned int base)
{
	*idr = (struct idr)IDR_INIT_BASE(idr, base);
}

static inline unsigned int idr_base(const struct idr *idr)
{
	return idr->idr_base;
}

static inline bool idr_is_empty(struct idr *idr)
{
	return xa_is_empty(&idr->idr_rt);
}

/*
 * The IDR API is a thin wrapper over the xarray API.  These inline
 * wrappers exist so that driver code written against the upstream
 * idr.h keeps compiling unchanged.  The underlying xarray operations
 * are provided by linuxu/src/xarray.c.
 */
int idr_alloc_cyclic(struct idr *idr, void *ptr, int start,
		     int end, gfp_t gfp);
int idr_alloc(struct idr *idr, void *ptr, int start,
	      int end, gfp_t gfp);
void *idr_get_next(struct idr *idr, int *next);
void *idr_get_next_ul(struct idr *idr, unsigned long *next);
void *idr_find(struct idr *idr, unsigned int id);
void *idr_full_find(struct idr *idr, unsigned int id);
void *idr_replace(struct idr *idr, void *ptr, unsigned int id);
void *idr_remove(struct idr *idr, unsigned int id);
void idr_destroy(struct idr *idr);
int idr_for_each(struct idr *idr, int (*fn)(int id, void *p, void *data),
		  void *data);
int idr_for_each_action(struct idr *idr, int action, void *(*fn)(int id,
			void *p, void *data), void *data);
#define IDR_FOR_EACH(idr, ptr, id) \
	idr_for_each((idr), idr_walk_fn_stub, &ptr)

/* The 2026 upstream idr.h dropped the gfp argument from idr_replace();
 * the driver calls it without gfp. Provide the current 3-arg form. */
static inline void *idr_replace_legacy(struct idr *idr, void *ptr,
					unsigned int id, gfp_t gfp)
{
	(void)gfp;
	return idr_replace(idr, ptr, id);
}

/* Match Linux sparse iteration, including holes and the continuation ID. */
#define idr_for_each_entry(idr, entry, id) \
	for ((id) = 0; ((entry) = idr_get_next((idr), (int *)&(id))) != NULL; (id) += 1U)
#define idr_for_each_entry_continue(idr, entry, id) \
	for (; ((entry) = idr_get_next((idr), (int *)&(id))) != NULL; (id) += 1U)
#define idr_for_each_entry_rcu(idr, entry, id, flags) \
	idr_for_each_entry(idr, entry, id)

static inline void idr_preload(gfp_t gfp) { (void)gfp; }
static inline void idr_preload_end(void) {}
#endif
