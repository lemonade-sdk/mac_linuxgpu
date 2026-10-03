/* linuxu shim: refcount — refcount_t ops
 * (REAL). The header inlines the fast paths over
 * atomic_t; __kref_put and friends live here (kref.c equivalent). */
#include <linux/refcount.h>
#include <linux/kref.h>
#include <linux/bug.h>

/* kref — same machinery, different wrapper type */
void __kref_put(struct kref *kref, unsigned int count,
		void (*release)(struct kref *))
{
	if (count && refcount_dec_and_test_many(&kref->refcount, count))
		release(kref);
}

/* refcount saturated-decrement helpers (upstream kernel semantics:
 * refuse to go negative, WARN at zero-dec). */
bool refcount_dec_if_not_zero_ext(refcount_t *r)
{
	return refcount_dec_if_not_zero(r);
}

/* note: refcount_t.count is atomic_t; the header's
 * refcount_dec_if_not_zero already covers the common path — this file
 * only needs to exist for link-time completeness. */
