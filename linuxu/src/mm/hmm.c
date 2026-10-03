/* HMM only resolves explicitly registered driver-owned pages. Imported client
 * process memory requires an OS pinning and invalidation backend. */
#include <pthread.h>
#include <stdlib.h>
#include <linux/hmm.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/errno.h>
#include <linux/bug.h>
#include <rt/fatal.h>

struct tracked_range {
	struct tracked_range *next;
	struct mm_struct *mm;
	unsigned long start, end, count;
	struct page **pages;
	bool writable, invalidating;
};
struct tracked_notifier {
	struct tracked_notifier *next;
	struct mmu_interval_notifier *notifier;
	unsigned int active;
};
static pthread_mutex_t hmm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hmm_changed = PTHREAD_COND_INITIALIZER;
static struct tracked_range *tracked_ranges;
static struct tracked_notifier *tracked_notifiers;
static unsigned int invalidations;

static struct tracked_range *find_range(struct mm_struct *mm, unsigned long start,
		unsigned long end)
{
	for (struct tracked_range *range = tracked_ranges; range; range = range->next)
		if (range->mm == mm && range->start <= start && range->end >= end) return range;
	return NULL;
}
static struct tracked_notifier *find_notifier(struct mmu_interval_notifier *notifier)
{
	for (struct tracked_notifier *entry = tracked_notifiers; entry; entry = entry->next)
		if (entry->notifier == notifier) return entry;
	return NULL;
}
int linuxu_hmm_register_pages(struct mm_struct *mm, unsigned long start,
		struct page *const *pages, unsigned long count, bool writable)
{
	if (!mm || !pages || !count || (start & (PAGE_SIZE - 1)) ||
		count > (ULONG_MAX - start) / PAGE_SIZE || count > SIZE_MAX / sizeof(*pages)) return -EINVAL;
	struct tracked_range *range = calloc(1, sizeof(*range));
	if (!range) return -ENOMEM;
	range->pages = malloc(count * sizeof(*pages));
	if (!range->pages) { free(range); return -ENOMEM; }
	range->mm = mm; range->start = start; range->end = start + count * PAGE_SIZE;
	range->count = count; range->writable = writable;
	unsigned long pinned = 0;
	int error = -EINVAL;
	for (; pinned < count; pinned++) {
		if (!linuxu_get_page(pages[pinned])) goto failed;
		range->pages[pinned] = pages[pinned];
	}
	pthread_mutex_lock(&hmm_lock);
	for (struct tracked_range *entry = tracked_ranges; entry; entry = entry->next) {
		if (entry->mm == mm && start < entry->end && range->end > entry->start) {
			error = -EEXIST;
			pthread_mutex_unlock(&hmm_lock);
			goto failed;
		}
	}
	range->next = tracked_ranges; tracked_ranges = range;
	pthread_mutex_unlock(&hmm_lock);
	return 0;
failed:
	while (pinned) put_page(range->pages[--pinned]);
	free(range->pages); free(range);
	return error;
}

int linuxu_hmm_unregister_pages(struct mm_struct *mm, unsigned long start, unsigned long count)
{
	if (!mm || !count || (start & (PAGE_SIZE - 1)) || count > (ULONG_MAX - start) / PAGE_SIZE) return -EINVAL;
	unsigned long end = start + count * PAGE_SIZE;
	pthread_mutex_lock(&hmm_lock);
	struct tracked_range *range = find_range(mm, start, end);
	if (!range || range->start != start || range->end != end) {
		pthread_mutex_unlock(&hmm_lock); return -ENOENT;
	}
	if (range->invalidating) { pthread_mutex_unlock(&hmm_lock); return -EBUSY; }
	size_t nr = 0;
	for (struct tracked_notifier *entry = tracked_notifiers; entry; entry = entry->next) {
		struct mmu_interval_notifier *sub = entry->notifier;
		if (sub->mm == mm && sub->interval_tree.start < end && sub->interval_tree.last >= start) nr++;
	}
	struct tracked_notifier **subscriptions = nr ? calloc(nr, sizeof(*subscriptions)) : NULL;
	if (nr && !subscriptions) { pthread_mutex_unlock(&hmm_lock); return -ENOMEM; }
	range->invalidating = true; invalidations++;
	size_t n = 0;
	for (struct tracked_notifier *entry = tracked_notifiers; entry; entry = entry->next) {
		struct mmu_interval_notifier *sub = entry->notifier;
		if (sub->mm != mm || sub->interval_tree.start >= end || sub->interval_tree.last < start) continue;
		entry->active++;
		subscriptions[n++] = entry;
		__atomic_add_fetch(&sub->invalidate_seq, 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&hmm_lock);
	struct mmu_notifier_range invalidated = {
		.mm = mm, .start = start, .end = end,
		.flags = MMU_NOTIFIER_RANGE_BLOCKABLE, .event = MMU_NOTIFY_UNMAP,
	};
	int error = 0;
	for (n = 0; n < nr; n++) {
		struct mmu_interval_notifier *sub = subscriptions[n]->notifier;
		unsigned long sequence = __atomic_load_n(&sub->invalidate_seq, __ATOMIC_ACQUIRE);
		if (sub->ops->invalidate_start) {
			struct mmu_interval_notifier_finish *finish = NULL;
			bool result = sub->ops->invalidate_start(sub, &invalidated, sequence, &finish);
			if (finish) sub->ops->invalidate_finish(finish);
			if (!result) error = -EBUSY;
		} else if (!sub->ops->invalidate(sub, &invalidated, sequence)) error = -EBUSY;
	}
	pthread_mutex_lock(&hmm_lock);
	for (n = 0; n < nr; n++) subscriptions[n]->active--;
	if (!error) {
		struct tracked_range **link = &tracked_ranges;
		while (*link != range) link = &(*link)->next;
		*link = range->next;
	} else range->invalidating = false;
	invalidations--;
	pthread_cond_broadcast(&hmm_changed);
	pthread_mutex_unlock(&hmm_lock);
	free(subscriptions);
	if (!error) {
		for (unsigned long i = 0; i < range->count; i++) put_page(range->pages[i]);
		free(range->pages); free(range);
	}
	return error;
}

/* Upstream mn_itree_release: on exit every interval subscription of @mm sees
 * one blockable MMU_NOTIFY_RELEASE invalidation of the whole address space. */
static void hmm_release_mm(struct mm_struct *mm)
{
	pthread_mutex_lock(&hmm_lock);
	size_t nr = 0;
	for (struct tracked_notifier *entry = tracked_notifiers; entry; entry = entry->next)
		if (entry->notifier->mm == mm) nr++;
	struct tracked_notifier **subscriptions = nr ? calloc(nr, sizeof(*subscriptions)) : NULL;
	if (nr && !subscriptions) {
		pthread_mutex_unlock(&hmm_lock);
		LINUXU_FATAL("hmm: cannot release interval notifiers");
	}
	size_t n = 0;
	for (struct tracked_notifier *entry = tracked_notifiers; entry; entry = entry->next) {
		if (entry->notifier->mm != mm) continue;
		entry->active++;
		subscriptions[n++] = entry;
		__atomic_add_fetch(&entry->notifier->invalidate_seq, 1, __ATOMIC_RELEASE);
	}
	if (nr) invalidations++;
	pthread_mutex_unlock(&hmm_lock);
	const struct mmu_notifier_range released = {
		.mm = mm, .start = 0, .end = ULONG_MAX,
		.flags = MMU_NOTIFIER_RANGE_BLOCKABLE, .event = MMU_NOTIFY_RELEASE,
	};
	for (n = 0; n < nr; n++) {
		struct mmu_interval_notifier *sub = subscriptions[n]->notifier;
		unsigned long sequence = __atomic_load_n(&sub->invalidate_seq, __ATOMIC_ACQUIRE);
		bool ret;
		if (sub->ops->invalidate_start) {
			struct mmu_interval_notifier_finish *finish = NULL;
			ret = sub->ops->invalidate_start(sub, &released, sequence, &finish);
			if (ret && finish) sub->ops->invalidate_finish(finish);
		} else ret = sub->ops->invalidate(sub, &released, sequence);
		WARN_ON(!ret);
	}
	pthread_mutex_lock(&hmm_lock);
	for (n = 0; n < nr; n++) subscriptions[n]->active--;
	if (nr) invalidations--;
	pthread_cond_broadcast(&hmm_changed);
	pthread_mutex_unlock(&hmm_lock);
	free(subscriptions);
}
static void hmm_synchronize(void)
{
	pthread_mutex_lock(&hmm_lock);
	while (invalidations) pthread_cond_wait(&hmm_changed, &hmm_lock);
	pthread_mutex_unlock(&hmm_lock);
}

int mmu_interval_notifier_insert(struct mmu_interval_notifier *sub, struct mm_struct *mm,
		unsigned long start, unsigned long length, const struct mmu_interval_notifier_ops *ops)
{
	if (!sub || !mm || !ops || (!ops->invalidate && !ops->invalidate_start) ||
		(ops->invalidate_start && !ops->invalidate_finish) || !length || length > ULONG_MAX - start) return -EINVAL;
	struct tracked_notifier *entry = calloc(1, sizeof(*entry));
	if (!entry) return -ENOMEM;
	int error = 0;
	pthread_mutex_lock(&hmm_lock);
	struct tracked_range *range = find_range(mm, start, start + length);
	if (find_notifier(sub)) error = -EEXIST;
	else if (!range) error = -EOPNOTSUPP;
	else if (range->invalidating) error = -EAGAIN;
	if (!error) {
		sub->mm = mm; sub->ops = ops;
		sub->interval_tree.start = start; sub->interval_tree.last = start + length - 1;
		__atomic_store_n(&sub->invalidate_seq, 0, __ATOMIC_RELEASE);
		entry->notifier = sub; entry->next = tracked_notifiers; tracked_notifiers = entry;
	}
	pthread_mutex_unlock(&hmm_lock);
	if (error) { free(entry); return error; }
	/* Pairs with the mmdrop in mmu_interval_notifier_remove (upstream). */
	mmgrab(mm);
	__atomic_store_n(&linuxu_mmu_interval_synchronize, hmm_synchronize, __ATOMIC_RELEASE);
	__atomic_store_n(&linuxu_mmu_interval_release, hmm_release_mm, __ATOMIC_RELEASE);
	return 0;
}
int mmu_interval_notifier_insert_locked(struct mmu_interval_notifier *sub,
		struct mm_struct *mm, unsigned long start, unsigned long length,
		const struct mmu_interval_notifier_ops *ops)
{
	return mmu_interval_notifier_insert(sub, mm, start, length, ops);
}
void mmu_interval_notifier_remove(struct mmu_interval_notifier *sub)
{
	struct mm_struct *mm = NULL;
	pthread_mutex_lock(&hmm_lock);
	struct tracked_notifier *entry = find_notifier(sub);
	if (entry) {
		while (entry->active) pthread_cond_wait(&hmm_changed, &hmm_lock);
		struct tracked_notifier **link = &tracked_notifiers;
		while (*link != entry) link = &(*link)->next;
		*link = entry->next;
		__atomic_add_fetch(&sub->invalidate_seq, 1, __ATOMIC_RELEASE);
		mm = sub->mm;
		sub->mm = NULL;
	}
	pthread_mutex_unlock(&hmm_lock);
	free(entry);
	if (mm) mmdrop(mm);
}
unsigned long mmu_interval_read_begin(struct mmu_interval_notifier *sub)
{
	pthread_mutex_lock(&hmm_lock);
	struct tracked_notifier *entry;
	while ((entry = find_notifier(sub)) && entry->active) pthread_cond_wait(&hmm_changed, &hmm_lock);
	unsigned long result = sub ? __atomic_load_n(&sub->invalidate_seq, __ATOMIC_ACQUIRE) : 0;
	pthread_mutex_unlock(&hmm_lock);
	return result;
}

int linuxu_hmm_register_range(unsigned long start, unsigned long end)
{
	if (start >= end || (start & (PAGE_SIZE - 1)) || (end & (PAGE_SIZE - 1))) return -EINVAL;
	return -EOPNOTSUPP; /* A bare address has neither page ownership nor an mm. */
}
int linuxu_hmm_unregister_range(unsigned long start, unsigned long end)
{
	return linuxu_hmm_register_range(start, end);
}
int hmm_range_fault(struct hmm_range *range)
{
	if (!range || !range->hmm_pfns || range->start >= range->end ||
		(range->start & (PAGE_SIZE - 1)) || (range->end & (PAGE_SIZE - 1))) return -EINVAL;
	int error = 0;
	pthread_mutex_lock(&hmm_lock);
	struct tracked_notifier *entry = find_notifier(range->notifier);
	struct tracked_range *memory = entry ? find_range(range->notifier->mm, range->start, range->end) : NULL;
	if (!memory) error = -EOPNOTSUPP;
	else if (memory->invalidating || entry->active ||
		mmu_interval_read_retry(range->notifier, range->notifier_seq)) error = -EBUSY;
	else if (range->start < range->notifier->interval_tree.start ||
		range->end - 1 > range->notifier->interval_tree.last) error = -EINVAL;
	if (!error) {
		unsigned long count = (range->end - range->start) / PAGE_SIZE;
		unsigned long first = (range->start - memory->start) / PAGE_SIZE;
		for (unsigned long i = 0; i < count; i++) {
			unsigned long flags = (range->hmm_pfns[i] & range->pfn_flags_mask) | range->default_flags;
			if ((flags & HMM_PFN_REQ_WRITE) && !memory->writable) { error = -EFAULT; break; }
			range->hmm_pfns[i] = page_to_pfn(memory->pages[first + i]) | HMM_PFN_VALID |
				(memory->writable ? HMM_PFN_WRITE : 0) | (range->hmm_pfns[i] & HMM_PFN_DMA_MAPPED);
		}
	}
	pthread_mutex_unlock(&hmm_lock);
	return error;
}
void hmm_depopulate_page(struct page *page, int ptype) { (void)page; (void)ptype; }
