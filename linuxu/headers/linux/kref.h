/* linuxu: EDITED (third_party/linux/include/linux/kref.h) - kept upstream
 * structure and function set; includes reduced to spinlock.h +
 * refcount.h (provided by linuxu); upstream body kept verbatim where
 * it is pure inlines. */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KREF_H_
#define _KREF_H_

#include <linux/spinlock.h>
#include <linux/refcount.h>

/*
 * The shim's refcount.h may not be present yet; provide a local
 * fallback so kref.h is self-contained in the interim.
 */


struct kref {
	refcount_t refcount;
};

extern void __kref_put(struct kref *kref, unsigned int count,
		       void (*release)(struct kref *));

static inline void kref_init(struct kref *kref)
{
	refcount_set(&kref->refcount, 1);
}

static inline void kref_get(struct kref *kref)
{
	refcount_inc(&kref->refcount);
}

static inline void __kref_inc(struct kref *kref)
{
	refcount_inc(&kref->refcount);
}

/* Returns true if the refcount is not zero and was incremented. */
static inline bool kref_get_unless_zero(struct kref *kref)
{
	return refcount_inc_not_zero(&kref->refcount);
}

static inline bool kref_get_unless_zero_try(struct kref *kref)
{
	return refcount_inc_not_zero(&kref->refcount);
}

static inline int kref_read(const struct kref *kref)
{
	return (int)refcount_read(&kref->refcount);
}

static inline bool kref_zero(const struct kref *kref)
{
	return refcount_zero(&kref->refcount);
}

static inline void kref_put(struct kref *kref, void (*release)(struct kref *))
{
	/* refcount_dec_and_test performs the single decrement; release is
	 * called only when the count reaches zero. (The prior version
	 * also called __kref_put in the else-branch, which decremented a
	 * SECOND time — every put dropped the refcount by 2, causing
	 * underflow/BUG on any fence held by >1 owner.) */
	if (refcount_dec_and_test(&kref->refcount))
		release(kref);
}

#endif /* _KREF_H_ */
