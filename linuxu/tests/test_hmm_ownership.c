/* Registered shim pages only; this test never imports client memory. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <linux/hmm.h>
#include <linux/mmu_notifier.h>
#include <linux/mm.h>

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool invalidate_entered, allow_invalidate, reject_invalidate;
static struct page *pages[2];
static struct mm_struct *memory;
static unsigned int releases;
static unsigned int callbacks;
static int unregister_result;
static bool invalidate(struct mmu_interval_notifier *sub,
		const struct mmu_notifier_range *range, unsigned long seq)
{
	assert(range->mm == memory && mmu_notifier_range_blockable(range));
	if (range->event == MMU_NOTIFY_RELEASE) {
		/* Exit invalidates the whole address space (mn_itree_release). */
		assert(range->start == 0 && range->end == ULONG_MAX);
		mmu_interval_set_seq(sub, seq);
		releases++;
		return true;
	}
	assert(range->start == 4 * PAGE_SIZE && range->end == 6 * PAGE_SIZE);
	assert(page_address(pages[0]) && page_ref_count(pages[0]) == 1);
	mmu_interval_set_seq(sub, seq);
	pthread_mutex_lock(&test_lock);
	callbacks++; invalidate_entered = true;
	pthread_cond_broadcast(&changed);
	while (!allow_invalidate) pthread_cond_wait(&changed, &test_lock);
	pthread_mutex_unlock(&test_lock);
	return !reject_invalidate;
}
static void *unregister_pages(void *arg)
{
	(void)arg;
	unregister_result = linuxu_hmm_unregister_pages(memory, 4 * PAGE_SIZE, 2);
	return NULL;
}
int main(void)
{
	assert(!linuxu_page_pool_extend(16));
	memory = linuxu_mm_alloc();
	assert(memory);
	pages[0] = alloc_page(0); pages[1] = alloc_page(0);
	assert(pages[0] && pages[1]);
	assert(!linuxu_hmm_register_pages(memory, 4 * PAGE_SIZE, pages, 2, true));
	assert(linuxu_hmm_register_pages(memory, 4 * PAGE_SIZE, pages, 2, true) == -EEXIST);
	assert(page_ref_count(pages[0]) == 2);
	put_page(pages[0]); put_page(pages[1]);
	struct mmu_interval_notifier sub = {0}, foreign = {0};
	const struct mmu_interval_notifier_ops ops = { .invalidate = invalidate };
	assert(!mmu_interval_notifier_insert(&sub, memory, 4 * PAGE_SIZE, 2 * PAGE_SIZE, &ops));
	assert(mmu_interval_notifier_insert(&foreign, memory, PAGE_SIZE, PAGE_SIZE, &ops) == -EOPNOTSUPP);
	unsigned long pfns[2] = {HMM_PFN_DMA_MAPPED, 0};
	struct hmm_range range = {.notifier = &sub, .start = 4 * PAGE_SIZE, .end = 6 * PAGE_SIZE,
		.hmm_pfns = pfns, .default_flags = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE};
	range.notifier_seq = mmu_interval_read_begin(&sub);
	assert(!hmm_range_fault(&range));
	assert(hmm_pfn_to_page(pfns[0]) == pages[0] && hmm_pfn_to_page(pfns[1]) == pages[1]);
	assert((pfns[0] & (HMM_PFN_VALID | HMM_PFN_WRITE | HMM_PFN_DMA_MAPPED)) ==
		(HMM_PFN_VALID | HMM_PFN_WRITE | HMM_PFN_DMA_MAPPED));
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, unregister_pages, NULL));
	pthread_mutex_lock(&test_lock);
	while (!invalidate_entered) pthread_cond_wait(&changed, &test_lock);
	pthread_mutex_unlock(&test_lock);
	assert(mmu_interval_read_retry(&sub, range.notifier_seq));
	assert(hmm_range_fault(&range) == -EBUSY);
	assert(page_address(pages[0]));
	pthread_mutex_lock(&test_lock);
	reject_invalidate = true; allow_invalidate = true;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&test_lock);
	pthread_join(worker, NULL);
	assert(unregister_result == -EBUSY && callbacks == 1 && page_address(pages[0]));
	range.notifier_seq = mmu_interval_read_begin(&sub);
	assert(!hmm_range_fault(&range));
	reject_invalidate = false;
	assert(!linuxu_hmm_unregister_pages(memory, 4 * PAGE_SIZE, 2));
	assert(callbacks == 2 && !page_address(pages[0]) && !page_address(pages[1]));
	assert(hmm_range_fault(&range) == -EOPNOTSUPP);
	mmu_interval_notifier_remove(&sub);
	mmu_notifier_synchronize();
	/* An interval subscription pins the mm until it is removed. */
	assert(atomic_read(&memory->mm_count) == 1);

	pages[0] = alloc_page(0);
	assert(!linuxu_hmm_register_pages(memory, 4 * PAGE_SIZE, pages, 1, false));
	assert(!mmu_interval_notifier_insert(&sub, memory, 4 * PAGE_SIZE, PAGE_SIZE, &ops));
	range.end = 5 * PAGE_SIZE; range.notifier_seq = mmu_interval_read_begin(&sub);
	assert(hmm_range_fault(&range) == -EFAULT);
	range.default_flags = HMM_PFN_REQ_FAULT;
	assert(!hmm_range_fault(&range) && !(pfns[0] & HMM_PFN_WRITE));
	assert(atomic_read(&memory->mm_count) == 2);
	/* exit_mmap: interval subscriptions see one MMU_NOTIFY_RELEASE, and the
	 * retry sequence changes so readers notice. */
	unsigned long seq = mmu_interval_read_begin(&sub);
	linuxu_mm_exit(memory);
	linuxu_mm_exit(memory);
	assert(releases == 1 && mmu_interval_read_retry(&sub, seq));
	mmu_interval_notifier_remove(&sub);
	assert(!linuxu_hmm_unregister_pages(memory, 4 * PAGE_SIZE, 1));
	assert(page_ref_count(pages[0]) == 1);
	put_page(pages[0]);
	assert(atomic_read(&memory->mm_count) == 1);
	mmput(memory);
	puts("HMM: explicit page ownership, PFNs, write protection, overlapping invalidation, retry, refused revocation, mm pinning and exit release passed");
}
