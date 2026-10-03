/* linuxu: EDITED (third_party/linux/include/linux/radix-tree.h)
 *
 * Upstream is a compat shim over xarray; keep the same shape:
 * radix_tree_root == xarray alias + the radix_* wrappers. Per-CPU preload
 * state is stripped (no SMP in shim); all operations route to the xarray
 * runtime (linuxu/src/xarray.c).
 */
#ifndef _LINUX_RADIX_TREE_H
#define _LINUX_RADIX_TREE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/xarray.h>

/* Keep unconverted code working */
#define radix_tree_root		xarray
#define radix_tree_node		xa_node

struct radix_tree_preload {
	unsigned int		nr;
	/* nodes->parent points to next preallocated node */
	struct radix_tree_node *nodes;
};

#define RADIX_TREE_ENTRY_MASK		3UL
#define RADIX_TREE_INTERNAL_NODE	2UL

static inline bool radix_tree_is_internal_node(void *ptr)
{
	return ((unsigned long)ptr & RADIX_TREE_ENTRY_MASK) ==
				RADIX_TREE_INTERNAL_NODE;
}

#define RADIX_TREE_MAP_SHIFT	XA_CHUNK_SHIFT
#define RADIX_TREE_MAP_SIZE	(1UL << RADIX_TREE_MAP_SHIFT)
#define RADIX_TREE_MAP_MASK	(RADIX_TREE_MAP_SIZE - 1)

#define RADIX_TREE_MAX_TAGS	XA_MAX_MARKS
#define RADIX_TREE_TAG_LONGS	XA_MARK_LONGS

#define RADIX_TREE_INDEX_BITS	(8 * sizeof(unsigned long))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define RADIX_TREE_MAX_PATH (DIV_ROUND_UP(RADIX_TREE_INDEX_BITS, \
					  RADIX_TREE_MAP_SHIFT))

/* The IDR tag is stored in the low bits of xa_flags */
#define ROOT_IS_IDR		((gfp_t)4)
/* The top bits of xa_flags are used to store the root tags */
#define ROOT_TAG_SHIFT		30

#define RADIX_TREE_INIT(name, mask)	XARRAY_INIT(name, mask)

#define RADIX_TREE(name, mask) \
	struct radix_tree_root name = RADIX_TREE_INIT(name, mask)

#define INIT_RADIX_TREE(root, mask) xa_init_flags(root, mask)

static inline bool radix_tree_empty(const struct radix_tree_root *root)
{
	return __atomic_load_n(&root->xa_head, __ATOMIC_ACQUIRE) == NULL;
}

struct radix_tree_iter {
	unsigned long	index;
	unsigned long	next_index;
	unsigned long	tags;
	struct radix_tree_node *node;
};

/**
 * radix_tree_deref_slot - dereference a slot
 */
static inline void *radix_tree_deref_slot(void **slot)
{
	return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

static inline void *radix_tree_deref_protected(void **slot,
						unsigned long tree_lock)
{
	return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

static inline int radix_tree_deref_retry(void *arg)
{
	return radix_tree_is_internal_node(arg);
}

static inline int radix_tree_exception(void *arg)
{
	return (unsigned long)arg & RADIX_TREE_ENTRY_MASK;
}

/* Runtime-implemented (linuxu/src/xarray.c): */
int radix_tree_insert(struct radix_tree_root *, unsigned long index,
		      void *);
void *__radix_tree_lookup(const struct radix_tree_root *, unsigned long index,
			  struct radix_tree_node **nodep, void ***slotp);
void *radix_tree_lookup(const struct radix_tree_root *, unsigned long);
void **radix_tree_lookup_slot(const struct radix_tree_root *,
			      unsigned long index);
void radix_tree_replace_slot(struct radix_tree_root *, void **slot, void *);
void radix_tree_iter_replace(struct radix_tree_root *,
			     const struct radix_tree_iter *, void **slot,
			     void *entry);
void *radix_tree_delete(struct radix_tree_root *, unsigned long index);
int radix_tree_gang_lookup(struct radix_tree_root *, void **, unsigned long,
			   unsigned int);
int radix_tree_gang_lookup_tag(struct radix_tree_root *, void **, unsigned long,
			       unsigned int, unsigned long tag);
int radix_tree_gang_lookup_tag_slot(const struct radix_tree_root *, void **,
				    unsigned long, unsigned int,
				    unsigned long tag);
int radix_tree_shrink(struct radix_tree_root *);
void radix_tree_tag_set(struct radix_tree_root *, unsigned long index,
			unsigned long tag);
void radix_tree_tag_clear(struct radix_tree_root *, unsigned long index,
			  unsigned long tag);
int radix_tree_tag_get(struct radix_tree_root *, unsigned long index,
		       unsigned long tag);
int radix_tree_tagged(struct radix_tree_root *, unsigned long tag);
unsigned long radix_tree_next_chunk(const struct radix_tree_root *,
				    unsigned long *iter);
int radix_tree_preload(gfp_t gfp_mask);
int radix_tree_iter_tag_clear(struct radix_tree_root *,
			      struct radix_tree_iter *iter,
			      void **slot, unsigned long tag);
int radix_tree_iter_tag_set(struct radix_tree_root *,
			    struct radix_tree_iter *iter, void **slot,
			    unsigned long tag);
int radix_tree_iter_set(struct radix_tree_root *,
			struct radix_tree_iter *iter, void **slot, void *entry);
int radix_tree_iter_delete(struct radix_tree_root *, struct radix_tree_iter *iter,
			   void **slot);
int radix_tree_iter_retry(struct radix_tree_iter *iter);
int radix_tree_iter_try_init(struct radix_tree_iter *iter,
			     struct radix_tree_root *root,
			     unsigned long tag, unsigned long index);
void radix_tree_iter_init(struct radix_tree_iter *iter,
			  struct radix_tree_root *root,
			  unsigned long tag, unsigned long index);
int radix_tree_iter_next(struct radix_tree_iter *iter, void ***slotp);
int radix_tree_iter_next_chunk(struct radix_tree_iter *iter, void **slotp);

/* Slots point into a stable leaf while the caller holds its tree lock.
 * Advance by index so deleting the current entry cannot invalidate iteration. */
void **linuxu_radix_iter_first(struct radix_tree_root *root,
		struct radix_tree_iter *iter, unsigned long start);
void **linuxu_radix_iter_next(struct radix_tree_root *root,
		struct radix_tree_iter *iter);
#define radix_tree_for_each_slot(slot, root, iter, start) \
	for ((slot) = linuxu_radix_iter_first((root), (struct radix_tree_iter *)(iter), (start)); \
	     (slot); (slot) = linuxu_radix_iter_next((root), (struct radix_tree_iter *)(iter)))

#endif /* _LINUX_RADIX_TREE_H */
