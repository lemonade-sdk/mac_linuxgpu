/* Unchanged GART dummy-page and TTM pool ownership against mocked DriverKit
 * allocation calls. No IOKit service or installed driver is contacted. */
#define main page_fixture_main
#define num_possible_nodes page_fixture_num_possible_nodes
#include "test_page_alloc_dk.c"
#undef main
#undef num_possible_nodes
#include <linux/list_lru.h>
#include <linux/mmzone.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/vmalloc.h>
#include <drm/ttm/ttm_pool.h>
#include "ttm_pool_internal.h"

/* Only the two fields touched by the extracted GART helpers are needed. */
struct amdgpu_device { struct pci_dev *pdev; dma_addr_t dummy_page_addr; };
static struct { struct page *dummy_read_page; } ttm_glob;
struct ttm_pool_dma { dma_addr_t addr; unsigned long vaddr; };
static atomic_long_t allocated_pages[1];
static spinlock_t shrinker_lock;
static struct list_head shrinker_list;
#undef dev_err
#define dev_err(...) do { } while (0)
#include "upstream-dma-pool.inc"

static void dummy_lifetime(void)
{
	struct pci_dev pdev = {0};
	u64 mask = DMA_BIT_MASK(44);
	pdev.dev.dma_mask = &mask;
	pdev.dev.coherent_dma_mask = mask;
	struct amdgpu_device adev = {.pdev = &pdev};
	for (int round = 0; round < 4; round++) {
		ttm_glob.dummy_read_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		assert(ttm_glob.dummy_read_page && backing_live == 1);
		assert(!amdgpu_gart_dummy_page_init(&adev));
		dma_addr_t first = adev.dummy_page_addr;
		assert(first && first != (uintptr_t)page_address(ttm_glob.dummy_read_page));
		assert(!amdgpu_gart_dummy_page_init(&adev));
		assert(first == adev.dummy_page_addr && backing_live == 1);
		/* Test both owner-release orders: the alias retains the DMA mapping
		 * if the page owner retires first. */
		if (round & 1) put_page(ttm_glob.dummy_read_page);
		assert(backing_live == 1);
		amdgpu_gart_dummy_page_fini(&adev);
		amdgpu_gart_dummy_page_fini(&adev);
		if (!(round & 1)) put_page(ttm_glob.dummy_read_page);
		assert(!adev.dummy_page_addr && !backing_live);
		assert(!linuxu_dart_used() && !linuxu_dart_table_count());
	}
	ttm_glob.dummy_read_page = NULL;
}

static void ttm_pool_lifetime(void)
{
	struct device dev = {.coherent_dma_mask = DMA_BIT_MASK(44)};
	struct ttm_pool pool = {.dev = &dev, .nid = 0,
		.alloc_flags = TTM_ALLOCATION_POOL_USE_DMA_ALLOC};
	spin_lock_init(&shrinker_lock);
	INIT_LIST_HEAD(&shrinker_list);
	for (unsigned order = 0; order <= 2; order++) {
		struct ttm_pool_type *pt = &pool.caching[ttm_cached].orders[order];
		ttm_pool_type_init(pt, &pool, ttm_cached, order);
		assert(pt->pages.node && !list_lru_count(&pt->pages));
		struct page *p = ttm_pool_alloc_page(&pool, GFP_KERNEL, order);
		assert(p && backing_live == 1);
		memset(page_address(p), 0xa7, PAGE_SIZE << order);
		ttm_pool_type_give(pt, p);
		assert(list_lru_count(&pt->pages) == 1);
		assert(pt->pages.nr_items == 1 && pt->pages.node->nr_items == 1);
		assert(atomic_long_read(&allocated_pages[0]) == 1L << order);
		for (size_t i = 0; i < (PAGE_SIZE << order); i++)
			assert(!((unsigned char *)page_address(p))[i]);
		assert(ttm_pool_type_take(pt, 0) == p);
		assert(!list_lru_count(&pt->pages) && !atomic_long_read(&allocated_pages[0]));
		assert(!ttm_pool_type_take(pt, 0));
		ttm_pool_type_give(pt, p);
		ttm_pool_type_fini(pt);
		assert(list_empty(&shrinker_list) && !list_lru_count(&pt->pages));
		assert(!backing_live && !linuxu_dart_used() && !linuxu_dart_table_count());
		assert(!atomic_long_read(&allocated_pages[0]));
	}
}

struct test_item { struct list_head link; unsigned seen; };
static enum lru_status exercise_status(struct list_head *item,
	struct list_lru_one *node, void *arg)
{
	struct test_item *entry = container_of(item, struct test_item, link);
	enum lru_status status = *(enum lru_status *)arg;
	entry->seen++;
	assert(spin_is_locked(&node->lock));
	if (status == LRU_REMOVED || status == LRU_REMOVED_RETRY)
		list_lru_isolate(node, item);
	if (status == LRU_RETRY || status == LRU_REMOVED_RETRY) {
		spin_unlock(&node->lock);
		spin_lock(&node->lock);
	}
	return status;
}
static void lru_statuses(void)
{
	for (enum lru_status status = LRU_REMOVED; status <= LRU_STOP; status++) {
		struct list_lru lru;
		struct test_item items[3] = {0};
		assert(!list_lru_init(&lru));
		for (unsigned i = 0; i < 3; i++) {
			INIT_LIST_HEAD(&items[i].link);
			assert(list_lru_add(&lru, &items[i].link, 0, NULL));
			assert(!list_lru_add(&lru, &items[i].link, 0, NULL));
		}
		assert(!list_lru_count_node(&lru, 1));
		unsigned long nr = 2;
		unsigned long removed = list_lru_walk_node(&lru, 0, exercise_status, &status, &nr);
		assert(removed == ((status == LRU_REMOVED || status == LRU_REMOVED_RETRY) ? 2 : 0));
		assert(nr == (status == LRU_STOP ? 1 : 0));
		assert(list_lru_count(&lru) == 3 - removed);
		assert(lru.nr_items == lru.node->nr_items);
		for (unsigned i = 0; i < 3; i++) {
			list_lru_del(&lru, &items[i].link, 0, NULL);
			assert(!list_lru_del(&lru, &items[i].link, 0, NULL));
		}
		assert(!list_lru_count(&lru));
		list_lru_destroy(&lru);
		assert(!lru.node);
	}
}

struct lru_producer {
	struct list_lru *lru;
	struct test_item items[128];
};
static void *produce_lru(void *arg)
{
	struct lru_producer *producer = arg;
	for (unsigned i = 0; i < ARRAY_SIZE(producer->items); i++) {
		INIT_LIST_HEAD(&producer->items[i].link);
		assert(list_lru_add(producer->lru, &producer->items[i].link, 0, NULL));
		sched_yield();
	}
	return NULL;
}
static void lru_concurrency(void)
{
	struct list_lru lru;
	struct lru_producer producers[4] = {0};
	pthread_t workers[4];
	assert(!list_lru_init(&lru));
	for (unsigned i = 0; i < ARRAY_SIZE(workers); i++) {
		producers[i].lru = &lru;
		assert(!pthread_create(&workers[i], NULL, produce_lru, &producers[i]));
	}
	unsigned long removed = 0;
	enum lru_status status = LRU_REMOVED;
	for (unsigned attempt = 0; removed != 512 && attempt < 1000000; attempt++) {
		removed += list_lru_walk(&lru, exercise_status, &status, 7);
		sched_yield();
	}
	assert(removed == 512);
	for (unsigned i = 0; i < ARRAY_SIZE(workers); i++) {
		assert(!pthread_join(workers[i], NULL));
		for (unsigned j = 0; j < ARRAY_SIZE(producers[i].items); j++) {
			assert(producers[i].items[j].seen == 1);
			assert(list_empty(&producers[i].items[j].link));
		}
	}
	assert(!list_lru_count(&lru) && !lru.nr_items && !lru.node->nr_items);
	list_lru_destroy(&lru);
}
int main(void)
{
	dummy_lifetime(); ttm_pool_lifetime(); lru_statuses(); lru_concurrency();
	puts("upstream DMA pool: GART dummy alias release, TTM DMA pool reuse/fini and LRU callback states passed");
}
