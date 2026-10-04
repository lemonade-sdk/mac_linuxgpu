/* Real sysinfo adapter with read-only platform mocks and unchanged TTM math. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/sysinfo.h>
#include <linux/err.h>
#include <drm/ttm/ttm_device.h>
#include <rt/dart.h>

static const char *scenario;
static const uint64_t capacity = 64ull << 30;
static unsigned capacity_queries, page_queries;
static bool free_query_failure;
static uint32_t host_page_size = 16384;
static uint32_t reported_free_pages = 4096;

static int platform_query(const char *name, void *out, size_t *length,
			  void *replacement, size_t replacement_length)
{
	assert(!replacement && !replacement_length && out && length);
	if (!strcmp(name, "hw.memsize")) {
		__atomic_add_fetch(&capacity_queries, 1, __ATOMIC_RELAXED);
		assert(*length == sizeof(uint64_t));
		if (!strcmp(scenario, "denied")) return 1;
		uint64_t bytes = !strcmp(scenario, "zero") ? 0 : capacity;
		memcpy(out, &bytes, sizeof(bytes));
		if (!strcmp(scenario, "truncated")) *length = sizeof(uint32_t);
	} else if (!strcmp(name, "hw.pagesize")) {
		__atomic_add_fetch(&page_queries, 1, __ATOMIC_RELAXED);
		assert(*length == sizeof(uint32_t));
		uint32_t bytes = !strcmp(scenario, "bad-page") ? 12345 : host_page_size;
		memcpy(out, &bytes, sizeof(bytes));
	} else {
		assert(!strcmp(name, "vm.page_free_count") && *length == sizeof(uint32_t));
		if (free_query_failure) return 1;
		uint32_t pages = !strcmp(scenario, "free-overflow") ? UINT32_MAX : reported_free_pages;
		memcpy(out, &pages, sizeof(pages));
	}
	return 0;
}
int IOSysCtlByName(const char *name, void *out, size_t *length, void *replacement, size_t count)
{ return platform_query(name, out, length, replacement, count); }
int test_sysctlbyname(const char *name, void *out, size_t *length, void *replacement, size_t count)
{ return platform_query(name, out, length, replacement, count); }

static void *read_stats(void *unused)
{
	(void)unused;
	struct sysinfo info;
	memset(&info, 0xa5, sizeof(info));
	assert(si_meminfo(&info) == 0);
	assert(info.totalram == capacity / PAGE_SIZE && info.mem_unit == PAGE_SIZE);
	assert(info.freeram == (uint64_t)reported_free_pages * host_page_size / PAGE_SIZE);
	assert(!info.totalhigh && !info.freehigh && !info.bufferram && !info.totalswap);
	pg_data_t *node = NODE_DATA(0);
	assert(node && node->per_node_pages == info.totalram);
	unsigned long managed = 0;
	for (unsigned i = 0; i < MAX_NR_ZONES; i++) {
		assert(node->node_zones[i].zone_id == i && node->node_zones[i].zone_pgdat == node);
		managed += zone_managed_pages(&node->node_zones[i]);
	}
	assert(managed == info.totalram);
	return NULL;
}

static struct page dummy;
static unsigned dummy_pages, pools;
static unsigned long pool_pages;
static unsigned long ttm_pages_limit, ttm_dma32_pages_limit;
static int ttm_global_mutex;
static unsigned ttm_glob_use_count;
struct ttm_global ttm_glob;
struct dentry *ttm_debugfs_root;
static inline u64 ttm_get_node_memory_size(int nid);
int ttm_pool_mgr_init(unsigned long pages)
{
	assert(pages == capacity / PAGE_SIZE / 2);
	pool_pages = ttm_get_node_memory_size(0) / PAGE_SIZE / 2;
	assert(pool_pages == pages);
	pools++;
	return 0;
}
void ttm_pool_mgr_fini(void) { assert(pools == 1); pools--; }
struct page *alloc_pages(gfp_t flags, unsigned int order)
{ (void)flags; assert(!order && !dummy_pages); dummy_pages++; return &dummy; }
void __free_pages(struct page *page, unsigned int order)
{ assert(page == &dummy && !order && dummy_pages == 1); dummy_pages--; }
#undef mutex_lock
#undef mutex_unlock
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
#define debugfs_create_dir(...) NULL
#define debugfs_remove(...) ((void)0)
#define debugfs_create_atomic_t(...) ((void)0)
#define debugfs_create_file(...) NULL
#define pr_warn(...) assert(0)
#include "upstream-memory.inc"

int main(int argc, char **argv)
{
	assert(argc == 2);
	scenario = argv[1];
	_Static_assert(PAGE_SIZE == 16384, "TTM page unit");
	assert(si_meminfo(NULL) == -EINVAL && !NODE_DATA(-1) && !NODE_DATA(1));
	if (!strcmp(scenario, "retry")) {
		scenario = "denied";
		assert(linuxu_sysinfo_init() == -EIO);
		scenario = "valid";
		capacity_queries = 0;
	}
	bool invalid = !strcmp(scenario, "denied") || !strcmp(scenario, "truncated") ||
		!strcmp(scenario, "zero") || !strcmp(scenario, "bad-page") || !strcmp(scenario, "free-overflow");
	if (invalid) {
		struct sysinfo info, empty = {0};
		memset(&info, 0xa5, sizeof(info));
		assert(linuxu_sysinfo_init() < 0 && si_meminfo(&info) < 0);
		assert(!memcmp(&info, &empty, sizeof(info)) && !NODE_DATA(0));
		return 0;
	}
	if (!strcmp(scenario, "native4k")) host_page_size = 4096;
	pthread_t readers[12];
	for (unsigned i = 0; i < 12; i++) assert(!pthread_create(&readers[i], NULL, read_stats, NULL));
	for (unsigned i = 0; i < 12; i++) assert(!pthread_join(readers[i], NULL));
	assert(capacity_queries == 1 && page_queries == 1);
	reported_free_pages /= 2;
	read_stats(NULL);
	assert(!ttm_global_init() && ttm_glob_use_count == 1 && dummy_pages == 1 && pools == 1);
	assert(ttm_tt_pages_limit() == capacity / PAGE_SIZE / 2);
	assert((ttm_tt_pages_limit() << PAGE_SHIFT) == capacity / 2);
	assert(ttm_dma32_pages_limit == (2ul << 30) / PAGE_SIZE);
	assert(!ttm_global_init() && ttm_glob_use_count == 2 && dummy_pages == 1 && pools == 1);
	ttm_global_release(); assert(ttm_glob_use_count == 1 && dummy_pages == 1);
	ttm_global_release(); assert(!ttm_glob_use_count && !dummy_pages && !pools);
	if (!strcmp(scenario, "free-failure")) {
		struct sysinfo info;
		free_query_failure = true;
		memset(&info, 0xa5, sizeof(info));
		assert(si_meminfo(&info) == -EIO && !info.freeram);
		assert(info.totalram == capacity / PAGE_SIZE && info.mem_unit == PAGE_SIZE);
	}
	puts("sysinfo: page units, failure initialization, node topology and unchanged TTM limits passed");
}
