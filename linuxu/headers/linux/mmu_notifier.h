/* linuxu: SHIM (third_party/linux/include/linux/mmu_notifier.h)
 *
 * Classic notifiers (mmu_notifier_get/put/register/unregister and exit
 * ->release) follow upstream: linuxu/src/mm/mmu_notifier.c. Interval
 * notifiers track explicitly registered, driver-owned page ranges
 * (linuxu/src/mm/hmm.c); arbitrary client process memory needs a platform
 * import/invalidation API.
 */
#ifndef _LINUX_MMU_NOTIFIER_H
#define _LINUX_MMU_NOTIFIER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/interval_tree.h>
#include <linux/xarray.h>
#include <linux/err.h>

struct mm_struct;
struct device;

/* ---- interval-tree seq helpers (upstream shape) ---- */
struct mmu_notifier_seq {
	unsigned long seq;
};


/* ---- event enum ---- */
enum mmu_notifier_event {
	MMU_NOTIFY_UNMAP = 0,
	MMU_NOTIFY_CLEAR,
	MMU_NOTIFY_PROTECTION_VMA,
	MMU_NOTIFY_PROTECTION_PAGE,
	MMU_NOTIFY_SOFT_DIRTY,
	MMU_NOTIFY_RELEASE,
	MMU_NOTIFY_MIGRATE,
	MMU_NOTIFY_EXCLUSIVE,
	/* legacy shim names (kept for files that already used them) */
	MMU_NOTIFIER_RELEASE = MMU_NOTIFY_RELEASE,
	MMU_NOTIFIER_CLEAR_YOUNG = MMU_NOTIFY_CLEAR,
	MMU_NOTIFIER_TEST_YOUNG = MMU_NOTIFY_CLEAR,
	MMU_NOTIFIER_INVALIATE_RANGE = MMU_NOTIFY_UNMAP,
};

#define MMU_NOTIFIER_RANGE_BLOCKABLE (1 << 0)

/* ---- range / finish / ops ---- */
struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	unsigned flags;
	enum mmu_notifier_event event;
	void *owner;
};

struct mmu_interval_notifier_finish;

struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *interval_sub,
			   const struct mmu_notifier_range *range,
			   unsigned long cur_seq);
	bool (*invalidate_start)(struct mmu_interval_notifier *interval_sub,
				 const struct mmu_notifier_range *range,
				 unsigned long cur_seq,
				 struct mmu_interval_notifier_finish **finish);
	void (*invalidate_finish)(struct mmu_interval_notifier_finish *finish);
};

/* ---- classic (non-interval) notifier ops (upstream mmu_notifier.h) ----
 * Runtime: linuxu/src/mm/mmu_notifier.c, ported from the pinned upstream
 * mm/mmu_notifier.c. Subscriptions hang off mm->notifier_subscriptions;
 * readers run under a global SRCU domain and mmu_notifier_put frees through
 * call_srcu (ops->free_notifier, then mmdrop). */
struct mmu_notifier;

struct mmu_notifier_ops {
	void (*release)(struct mmu_notifier *subscription, struct mm_struct *mm);
	int (*invalidate_range_start)(struct mmu_notifier *subscription,
				      const struct mmu_notifier_range *range);
	void (*invalidate_range_end)(struct mmu_notifier *subscription,
				     const struct mmu_notifier_range *range);
	struct mmu_notifier *(*alloc_notifier)(struct mm_struct *mm);
	void (*free_notifier)(struct mmu_notifier *subscription);
};

struct mmu_notifier {
	struct hlist_node hlist;
	const struct mmu_notifier_ops *ops;
	struct mm_struct *mm;
	struct rcu_head rcu;
	unsigned int users;
};

extern struct mmu_notifier *mmu_notifier_get_locked(
		const struct mmu_notifier_ops *ops, struct mm_struct *mm);
extern struct mmu_notifier *mmu_notifier_get(const struct mmu_notifier_ops *ops,
					     struct mm_struct *mm);
extern void mmu_notifier_put(struct mmu_notifier *subscription);
extern int mmu_notifier_register(struct mmu_notifier *subscription,
				 struct mm_struct *mm);
extern int __mmu_notifier_register(struct mmu_notifier *subscription,
				   struct mm_struct *mm);
extern void mmu_notifier_unregister(struct mmu_notifier *subscription,
				    struct mm_struct *mm);
extern void __mmu_notifier_release(struct mm_struct *mm);
extern void __mmu_notifier_subscriptions_destroy(struct mm_struct *mm);
/* Interval subscriptions of @mm receive MMU_NOTIFY_RELEASE (linuxu/src/mm/hmm.c
 * installs this when the first interval notifier is inserted). */
extern void (*linuxu_mmu_interval_release)(struct mm_struct *mm);
extern void (*linuxu_mmu_interval_synchronize)(void);

struct mmu_interval_notifier {
	struct interval_tree_node interval_tree;
	const struct mmu_interval_notifier_ops *ops;
	struct mm_struct *mm;
	struct hlist_node deferred_item;
	unsigned long invalidate_seq;
};

static inline void mmu_interval_set_seq(struct mmu_interval_notifier *interval_sub,
					 unsigned long seq)
{
	__atomic_store_n(&interval_sub->invalidate_seq, seq, __ATOMIC_RELEASE);
}

extern unsigned long mmu_interval_read_begin(struct mmu_interval_notifier *interval_sub);

static inline bool mmu_interval_read_retry(struct mmu_interval_notifier *interval_sub,
					    unsigned long cur_seq)
{
	return __atomic_load_n(&interval_sub->invalidate_seq, __ATOMIC_ACQUIRE) != cur_seq;
}

static inline bool mmu_interval_check_retry(struct mmu_interval_notifier *interval_sub,
					     unsigned long start_seq,
					     unsigned long cur_seq)
{
	(void)start_seq;
	return mmu_interval_read_retry(interval_sub, cur_seq);
}

static inline bool mmu_notifier_range_blockable(const struct mmu_notifier_range *range)
{
	return !!(range->flags & MMU_NOTIFIER_RANGE_BLOCKABLE);
}

static inline void mmu_notifier_range_init(struct mmu_notifier_range *range,
					   struct mm_struct *mm,
					   enum mmu_notifier_event event,
					   unsigned long start,
					   unsigned long end,
					   void *owner)
{
	range->mm = mm;
	range->start = start;
	range->end = end;
	range->event = event;
	range->owner = owner;
	range->flags = 0;
}

static inline void mmu_notifier_range_init_owner(struct mmu_notifier_range *range,
						  struct mm_struct *mm,
						  enum mmu_notifier_event event,
						  unsigned long start,
						  unsigned long end,
						  void *owner)
{
	mmu_notifier_range_init(range, mm, event, start, end, owner);
}

/* ---- runtime (linuxu/src/hmm/mmu_notifier.c) ---- */
extern int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub,
					struct mm_struct *mm,
					unsigned long start, unsigned long end,
					const struct mmu_interval_notifier_ops *ops);
extern int mmu_interval_notifier_insert_locked(
		struct mmu_interval_notifier *interval_sub,
		struct mm_struct *mm, unsigned long start,
		unsigned long length,
		const struct mmu_interval_notifier_ops *ops);
extern void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub);
extern void mmu_notifier_synchronize(void);
extern struct device *mmu_get_domain_for_dev(struct device *dev);
extern void *mmu_iova_to_phys(struct device *dev, dma_addr_t iova);

#endif /* _LINUX_MMU_NOTIFIER_H */
