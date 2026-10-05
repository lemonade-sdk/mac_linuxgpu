/* linuxu: SHIM (third_party/linux/include/linux/xarray.h)
 *
 * Sparse storage and iteration are implemented by linuxu/src/xarray.c.
 * The caller-visible lock protects reference acquisition around reads;
 * public mutations take that lock and the __xa_* variants require it.
 */
#ifndef _LINUX_XARRAY_H
#define _LINUX_XARRAY_H

#include <linux/types.h>
#include <stddef.h>
#include <linux/spinlock.h>
#include <linux/err.h>

struct list_lru;

#define BITS_PER_XA_VALUE	(BITS_PER_LONG - 1)

/*
 * Entry encodings (from upstream):
 * 00: pointer, 10: internal, x1: value/tagged pointer.
 */

/*
 * XA_INIT(name) - designator initializer for struct xarray (upstream form).
 */
#define XA_INIT(name) 	.name = { .xa_flags = 0 }

static inline void *xa_mk_value(unsigned long v)
{
	return (void *)((v << 1) | 1);
}

static inline unsigned long xa_to_value(const void *entry)
{
	return (unsigned long)entry >> 1;
}

static inline bool xa_is_value(const void *entry)
{
	return (unsigned long)entry & 1;
}

static inline void *xa_tag_pointer(void *p, unsigned long tag)
{
	return (void *)((unsigned long)p | tag);
}

static inline void *xa_untag_pointer(void *entry)
{
	return (void *)((unsigned long)entry & ~3UL);
}

#define XA_MAX_MARKS		3
#define XA_MARK_LONGS		1
#define XA_CHUNK_SHIFT		6

#define XA_ZERO_ENTRY		((257UL << 2) | 2)
#define XA_ERR_TO_VALUE(err)	(-(long)(err) - 1)
#define XA_VALUE_TO_ERR(val)	(-(long)((val) + 1))

#define XA_RETRY		ERR_PTR(-11)
#define XA_BUSY			ERR_PTR(-16)
#define XA_ENOSPACE		ERR_PTR(-28)
#define XA_SPLIT		ERR_PTR(-4093)
#define XA_RETRY_ENTRY		256

#define __xa_mk_sibling(shift) ((void *)((shift) << 2 | 2))
static inline void *xa_mk_sibling(int shift)
{
	return __xa_mk_sibling(shift);
}
static inline int xa_to_sibling(void *entry)
{
	return xa_is_value(entry) ? -1 : (int)(unsigned long)entry >> 2;
}

#define XA_MARK_BITS_SHIFT	30
#define XA_FLAGS_MASK		(3UL << XA_MARK_BITS_SHIFT)
#define XA_MARK_FLAGS(mask)	((mask) << XA_MARK_BITS_SHIFT)

/* xarray init flags (upstream, subset used by the driver) */
#define XA_FLAGS_ALLOC		(1UL << 0)
#define XA_FLAGS_ALLOC1		(1UL << 1)
#define XA_FLAGS_ACCOUNT	(1UL << 2)
#define XA_FLAGS_LOCK_IRQ	(1UL << 4)
#define XA_FLAGS_LOCK_IRQLCK	(1UL << 5)

/* xa_find() filter selecting every present entry (Linux xa_mark_t 8). The
 * runtime xa_find()/xa_find_after() treat any non-mark filter this way. */
#define XA_PRESENT		8U

#define XA_SET_MARK		1U
#define XA_CLEAR_MARK		2U

#define __XA_STATE(name, xarray, index, _flags, _internal)			\
	struct xa_state name = {						\
		.xs_parent = (unsigned long)(xarray),			\
		.xa_flags = (__force unsigned long)(_flags) | (_internal),	\
		.xa_index = (index),					\
		.xa_shift = XA_STATE_SHIFT,				\
		.xa_node = (xarray)->xa_head,				\
	}

/*
 * struct xa_state - state for iterating through a xarray.
 * xs_parent retains the owning xarray for the supported iterator subset.
 */
struct xa_state {
	unsigned long		xs_parent;
	unsigned long		xs_sibling;
	unsigned long		xs_tag;
	unsigned long		xa_index;
	unsigned long		xa_marks;
	int			xa_shift;
	int			xa_depth;
	unsigned long		xa_flags;
	struct xa_state		*xa_prev;
	struct xa_node		*xa_node;
	void			*xa_entry;
};

#define XA_STATE_SHIFT		(2 * (8 * sizeof(unsigned long) - 1))

#define XA_STATE(name, xarray, index) \
	__XA_STATE(name, xarray, index, 0, 0)

#define XA_STATE_IOV(xa, iter, index, n) \
	__XA_STATE(xa, (xa), index, 0, XA_STATE_IOV_INTERNAL)
#define XA_STATE_IOV_INTERNAL 1U

#define XA_STATE_IOV_END(xa) \
	do { (xa)->xa_flags &= ~XA_STATE_IOV_INTERNAL; } while (0)

#define XA_STATE_TO_ITER(xa) \
	({ (xa)->xa_index; })

#define XA_ITER_IOV		0

/*
 * struct xa_node - a node in the xarray tree.
 * Runtime radix node with per-entry marks at each leaf.
 */
struct xa_node {
	struct rcu_head rcu;
	unsigned long		flags;
	int			shift;
	struct xa_node		*parent;
	void			*entries[64];
	unsigned long		marks[XA_MAX_MARKS];
};

/*
 * struct xarray - the xarray root.
 * xa_lock serializes caller ownership changes; the runtime additionally
 * protects radix nodes during reads and mutations.
 */
struct xarray {
	spinlock_t		xa_lock;
	unsigned long		xa_flags;
	struct xa_node		*xa_head;
};

#define XA_LIMIT_LO(index, limit) (((index) > (limit)) ? (limit) : (index))
#define XA_LIMIT_HI(index, limit) (((index) < (limit)) ? (limit) : (index))

static inline bool xa_is_empty(struct xarray *xa)
{
	return !__atomic_load_n(&xa->xa_head, __ATOMIC_ACQUIRE);
}

/* vendor 2026: xa_empty() (bool). */
static inline bool xa_empty(const struct xarray *xa)
{
	return __atomic_load_n(&xa->xa_head, __ATOMIC_ACQUIRE) == NULL;
}

#define xarray_init(xa)		xarray_init_flags(xa, 0)
#define xarray_init_flags(xa, flags) do {			\
	spin_lock_init(&(xa)->xa_lock);			\
	(xa)->xa_head = NULL;					\
	(xa)->xa_flags = (flags);				\
} while (0)

/* Upstream xa_init/xa_init_flags (used by the driver). */
static inline void xa_init(struct xarray *xa)
{
	xarray_init(xa);
}

static inline void xa_init_flags(struct xarray *xa, unsigned long flags)
{
	xarray_init_flags(xa, flags);
}

/*
 * XARRAY_INIT - vendor 2026 initializer (the embedded xa_lock is
 * zero-initialized locks are supported; XA_INIT above is the designator
 * form for embedded arrays).
 */
#define XARRAY_INIT(name, mask) { .xa_flags = (mask) }
#define XARRAY(name, mask) struct xarray name = XARRAY_INIT(name, mask)
#define XARRAY_FLAGS(name, mask) struct xarray name = XARRAY_INIT(name, mask)

/*
 * DEFINE_XARRAY_FLAGS / DEFINE_XARRAY / DEFINE_XARRAY_ALLOC(1) - vendor
 * 2026 definitions (amdgpu_ids.c: DEFINE_XARRAY_FLAGS(amdgpu_pasid_xa,
 * XA_FLAGS_LOCK_IRQ | XA_FLAGS_ALLOC1)).
 */
#define DEFINE_XARRAY_FLAGS(name, flags) \
	struct xarray name = { .xa_flags = (flags) }

#define DEFINE_XARRAY(name) DEFINE_XARRAY_FLAGS(name, 0)
/* As upstream: ALLOC hands out indices from 0, ALLOC1 from 1. */
#define DEFINE_XARRAY_ALLOC(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC)
#define DEFINE_XARRAY_ALLOC1(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC1)

/*
 * struct xa_limit (vendor 2026, verbatim) + the limit-taking alloc
 * forms. sched_main.c / amdgpu_ids.c call the limit forms; the
 * plain max-based runtime form is kept for the rest of the driver.
 */
struct xa_limit {
	u32 max;
	u32 min;
};
#define XA_LIMIT(_min, _max) (struct xa_limit) { .min = _min, .max = _max }
#define xa_limit_32b	XA_LIMIT(0, UINT_MAX)
#define xa_limit_31b	XA_LIMIT(0, INT_MAX)
#define xa_limit_16b	XA_LIMIT(0, USHRT_MAX)

/* Runtime-implemented (linuxu/src/xarray.c): */
extern int xas_store(struct xa_state *xas, void *entry);
extern void *xas_load(struct xa_state *xas);
extern void *xas_find(struct xa_state *xas, unsigned long max);
extern void *xas_find_marked(struct xa_state *xas, unsigned long max,
			     unsigned int mark);
extern int xas_create_range(struct xa_state *xas, unsigned long max,
			    int order, gfp_t gfp);
extern void *xas_err(struct xa_state *xas);
extern int xa_err(const void *x);
extern bool xa_is_err(const void *x);
extern void *__xa_store(struct xarray *xa, unsigned long index, void *entry,
		      gfp_t gfp);
static inline void *xa_store(struct xarray *xa, unsigned long index,
			     void *entry, gfp_t gfp)
{
	if (!xa) return ERR_PTR(-EINVAL);
	spin_lock(&xa->xa_lock);
	void *result = __xa_store(xa, index, entry, gfp);
	spin_unlock(&xa->xa_lock);
	return result;
}
extern int xa_store_range(struct xarray *xa, unsigned long index,
			  unsigned long max, void *entry, gfp_t gfp);
extern int xa_store_marked(struct xarray *xa, unsigned long index,
			   void *entry, unsigned long mark, gfp_t gfp);
extern void *xa_load(struct xarray *xa, unsigned long index);
extern void *__xa_erase(struct xarray *xa, unsigned long index);
extern void *xa_erase(struct xarray *xa, unsigned long index);
extern int xa_set_mark(struct xarray *xa, unsigned long index,
		       unsigned long mark, gfp_t gfp);
extern void xa_clear_mark(struct xarray *xa, unsigned long index,
			  unsigned long mark);
extern bool xa_test_mark(struct xarray *xa, unsigned long index,
			 unsigned long mark);
extern int xa_set_mark_range(struct xarray *xa, unsigned long index,
			     unsigned long max, unsigned long mark, gfp_t gfp);
extern void xa_clear_mark_range(struct xarray *xa, unsigned long index,
				unsigned long max, unsigned long mark);
extern unsigned long xa_count(struct xarray *xa, unsigned long index,
			      unsigned long max);
extern int xa_alloc_limit(struct xarray *xa, u32 *indexp,
			  void *entry, struct xa_limit limit, gfp_t gfp);

/*
 * driver call sites use the limit form by default. The limit argument is
 * struct xa_limit in vendor 2026 (see xa_limit_32b/31b/16b & the
 * XA_LIMIT() compound-literal macro). The plain integer-max form of
 * upstream xa_alloc is kept as `xa_alloc_max` in the linuxu runtime
 * (linuxu/src/xarray.c) and exposed via XA_ALLOC_MAX for the unit test
 * and any future integer-max caller. `xa_alloc` (the macro below) is
 * the limit form for the driver.
 */
#define xa_alloc(xa, indexp, entry, limit, gfp) \
	xa_alloc_limit((xa), (indexp), (entry), (limit), (gfp))
#define XA_ALLOC_MAX(xa, indexp, entry, max, gfp) \
	xa_alloc_max((xa), (indexp), (entry), (unsigned long)(max), (gfp))
extern int xa_alloc_max(struct xarray *xa, unsigned long *indexp, void *entry,
			 unsigned long max, gfp_t gfp);
extern int xa_alloc_cyclic(struct xarray *xa, unsigned long *indexp,
			   void *entry, unsigned long max,
			   unsigned long *hintp, gfp_t gfp);
extern int xa_alloc_cyclic_max(struct xarray *xa, unsigned long *indexp,
			       void *entry, unsigned long max,
			       unsigned long *hintp, gfp_t gfp);
extern int xa_alloc_cycle(struct xarray *xa, unsigned long *indexp,
			  void *entry, unsigned long max, gfp_t gfp);
extern int xa_reserve(struct xarray *xa, unsigned long index, gfp_t gfp);
extern int xa_resv_set_mark(struct xarray *xa, unsigned long index,
			    unsigned long mark, gfp_t gfp);
extern int xa_reserve_alloc(struct xarray *xa, unsigned long *indexp,
			    void *entry, unsigned long max, gfp_t gfp);
extern int xa_destroy(struct xarray *xa);
extern void *xa_find(struct xarray *xa, unsigned long *indexp,
		     unsigned long max, unsigned int action);
extern void *xa_find_after(struct xarray *xa, unsigned long *indexp,
			   unsigned long max, unsigned int action);
extern int xa_alloc_cyclic_limit(struct xarray *xa, u32 *id, void *entry,
				 struct xa_limit limit, u32 *next, gfp_t gfp);
extern void *xa_find_marked(struct xarray *xa, unsigned long *indexp,
			    unsigned long max, unsigned int action,
			    unsigned int mark);
extern int xas_reset(struct xa_state *xas, unsigned long index);
extern void *xas_next(struct xa_state *xas);
extern void *xas_next_marked(struct xa_state *xas, unsigned int mark);
extern void *xas_next_entries(struct xa_state *xas, int *entsp);
extern void *xas_next_chunk(struct xa_state *xas, unsigned long size);
extern void *xas_prev(struct xa_state *xas);
extern void *xas_load(struct xa_state *xas);
extern int xas_set_err(struct xa_state *xas, int error);
extern int xas_store(struct xa_state *xas, void *entry);
extern int xas_set(struct xa_state *xas, unsigned long index);
extern void *xas_erase(struct xa_state *xas);
extern bool xa_is_value_entry(const struct xarray *xa, unsigned long index);
extern int xas_create(struct xa_state *xas, gfp_t gfp);
extern int xas_split(struct xa_state *xas, gfp_t gfp, unsigned long order);
extern int xas_alloc(struct xa_state *xas, void *entry, gfp_t gfp);
extern int xas_alloc_cyclic(struct xa_state *xas, void *entry,
			    unsigned long *hintp, gfp_t gfp);
extern int xas_alloc_cyclic_max(struct xa_state *xas, void *entry,
				unsigned long *hintp, gfp_t gfp);
extern int xas_alloc_reserve(struct xa_state *xas, void *entry, gfp_t gfp);
extern void *xas_fail(struct xa_state *xas);
extern void *xas_failed(struct xa_state *xas);

/* ---- IRQ-variant locking macros (vendor 2026, verbatim) ---- */
#define xa_lock(xa)		spin_lock(&(xa)->xa_lock)
#define xa_unlock(xa)		spin_unlock(&(xa)->xa_lock)
#define xa_lock_irq(xa)		spin_lock_irq(&(xa)->xa_lock)
#define xa_unlock_irq(xa)	spin_unlock_irq(&(xa)->xa_lock)
#define xa_lock_irqsave(xa, flags) \
	spin_lock_irqsave(&(xa)->xa_lock, flags)
#define xa_unlock_irqrestore(xa, flags) \
	spin_unlock_irqrestore(&(xa)->xa_lock, flags)

/* IRQ-locked operations (vendor 2026 inline forms). */
static inline void *xa_store_irq(struct xarray *xa, unsigned long index,
				 void *entry, gfp_t gfp)
{
	void *curr;

	xa_lock_irq(xa);
	curr = (void *)__xa_store(xa, index, entry, gfp);
	xa_unlock_irq(xa);

	return curr;
}

static inline void *xa_erase_irq(struct xarray *xa, unsigned long index)
{
	void *entry;

	xa_lock_irq(xa);
	entry = __xa_erase(xa, index);
	xa_unlock_irq(xa);

	return entry;
}

static inline int xa_alloc_cyclic_irq(struct xarray *xa, u32 *id, void *entry,
				      struct xa_limit limit, u32 *next,
				      gfp_t gfp)
{
	return xa_alloc_cyclic_limit(xa, id, entry, limit, next, gfp);
}

#define xas_lock(xas)		xa_lock(((struct xarray *)(xas)->xs_parent))
#define xas_unlock(xas)		xa_unlock(((struct xarray *)(xas)->xs_parent))
#define xas_lock_irq(xas)	xa_lock_irq(((struct xarray *)(xas)->xs_parent))
#define xas_unlock_irq(xas)	xa_unlock_irq(((struct xarray *)(xas)->xs_parent))
#define xas_lock_irqsave(xas, flags) \
	xa_lock_irqsave(((struct xarray *)(xas)->xs_parent), flags)
#define xas_unlock_irqrestore(xas, flags) \
	xa_unlock_irqrestore(((struct xarray *)(xas)->xs_parent), flags)

/* ---- iteration (backed by xa_find; XA_PRESENT/ANY = 0) ---- */
#define xa_for_each(xa, index, entry)					\
	for ((index) = 0, (entry) = xa_find(xa, &(index), -1UL, 0);	\
	     (entry) != NULL;							\
	     (entry) = xa_find_after(xa, &(index), -1UL, 0))

#define xa_for_each_range(xa, index, entry, start, last)			\
	for ((index) = (start),						\
	     (entry) = xa_find(xa, &(index), (last), 0);			\
	     (entry) != NULL;						\
	     (entry) = xa_find_after(xa, &(index), (last), 0))

#define xa_for_each_start(xa, index, entry, start)			\
	xa_for_each_range(xa, index, entry, (start), -1UL)

#endif /* _LINUX_XARRAY_H */
