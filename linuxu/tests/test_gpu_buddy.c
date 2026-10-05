/* Exercise the pinned allocator and actual VRAM-manager initialization body.
 * VRAM addresses are integers; the only allocations are CPU metadata. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <linux/gpu_buddy.h>
#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/overflow.h>
#include "dext_heap_backend.h"

extern int linuxu_module_init_gpu_buddy_module_init(void);
extern void linuxu_module_exit_gpu_buddy_module_exit(void);
static bool negative_control;

void linuxu_bug(const char *file, int line)
{ fprintf(stderr, "allocator BUG: %s:%d\n", file, line); abort(); }
void linuxu_warn(const char *file, int line, const char *format, ...)
{
	fprintf(stderr, "allocator WARN: %s:%d\n", file, line);
	/* The production warning hook logs and returns. Positive checks fail
	 * immediately; the negative control continues to its invalid access. */
	if (!negative_control) abort();
}
int printk(const char *format, ...)
{ (void)format; return 0; }

/* Only non-allocator interfaces in the extracted driver function are mocked. */
struct ttm_resource_manager { u64 size; void *cg; const void *func; bool used; };
struct amdgpu_vram_mgr {
	struct ttm_resource_manager manager;
	struct gpu_buddy mm;
	struct { bool initialized; } lock;
	struct list_head reservations_pending, reserved_pages, allocated_vres_list;
	u64 default_page_size;
};
struct amdgpu_device {
	struct { struct amdgpu_vram_mgr vram_mgr; int bdev; } mman;
	struct { u64 real_vram_size; } gmc;
};
#define TTM_PL_VRAM 2
static const int amdgpu_vram_mgr_func;
static struct ttm_resource_manager *registered;
static void *adev_to_drm(struct amdgpu_device *device) { return device; }
static void *drmm_cgroup_register_region(void *device, const char *name, u64 size)
{ (void)device; (void)name; (void)size; return NULL; }
static void ttm_resource_manager_init(struct ttm_resource_manager *manager, int *device, u64 size)
{ (void)device; manager->size = size; }
static void ttm_set_driver_manager(int *device, unsigned type, struct ttm_resource_manager *manager)
{ (void)device; assert(type == TTM_PL_VRAM); registered = manager; }
static void ttm_resource_manager_set_used(struct ttm_resource_manager *manager, bool used)
{ manager->used = used; }
#undef mutex_init
#define mutex_init(lock) ((lock)->initialized = true)
#include "vram-init.inc"
#undef mutex_init

static void module_start(void)
{ assert(linuxu_module_init_gpu_buddy_module_init() == 0); }
static void module_stop(void)
{
	linuxu_module_exit_gpu_buddy_module_exit();
	assert(dext_heap_test_live_allocations() == 0);
}

static u64 check_blocks(struct gpu_buddy *mm, struct list_head *blocks,
		       u64 begin, u64 end, bool contiguous)
{
	struct gpu_buddy_block *block;
	u64 total = 0, previous_end = begin;
	list_for_each_entry(block, blocks, link) {
		u64 start = gpu_buddy_block_offset(block), size = gpu_buddy_block_size(mm, block);
		assert(!gpu_buddy_block_is_free(block));
		assert(start >= begin && start < end && size <= end - start);
		assert(!(start % mm->chunk_size) && !(size % mm->chunk_size));
		if (contiguous && total) assert(start == previous_end);
		previous_end = start + size;
		total += size;
	}
	return total;
}

static void actual_vram_init(void)
{
	module_start();
	struct amdgpu_device device = {.gmc.real_vram_size = 32624ull * SZ_1M};
	assert(amdgpu_vram_mgr_init(&device) == 0);
	struct amdgpu_vram_mgr *manager = &device.mman.vram_mgr;
	assert(registered == &manager->manager && manager->manager.used);
	assert(manager->lock.initialized && manager->default_page_size == PAGE_SIZE);
	assert(list_empty(&manager->reserved_pages) && list_empty(&manager->reservations_pending));
	struct gpu_buddy *mm = &manager->mm;
	assert(mm->size == device.gmc.real_vram_size && mm->avail == mm->size);
	assert(mm->chunk_size == PAGE_SIZE && !mm->clear_avail);
	LIST_HEAD(low); LIST_HEAD(high); LIST_HEAD(duplicate);
	assert(!gpu_buddy_alloc_blocks(mm, 0, SZ_256M, SZ_2M, PAGE_SIZE, &low,
		GPU_BUDDY_RANGE_ALLOCATION | GPU_BUDDY_CONTIGUOUS_ALLOCATION));
	assert(check_blocks(mm, &low, 0, SZ_256M, true) == SZ_2M);
	u64 low_start = gpu_buddy_block_offset(list_first_entry(&low, struct gpu_buddy_block, link));
	assert(gpu_buddy_alloc_blocks(mm, low_start, low_start + SZ_2M, SZ_2M,
		PAGE_SIZE, &duplicate, GPU_BUDDY_RANGE_ALLOCATION) == -ENOSPC);
	assert(list_empty(&duplicate));
	assert(!gpu_buddy_alloc_blocks(mm, SZ_256M, mm->size, SZ_1M, PAGE_SIZE, &high,
		GPU_BUDDY_RANGE_ALLOCATION | GPU_BUDDY_TOPDOWN_ALLOCATION));
	assert(check_blocks(mm, &high, SZ_256M, mm->size, true) == SZ_1M);
	assert(mm->avail == mm->size - SZ_2M - SZ_1M);
	gpu_buddy_free_list(mm, &low, 0); gpu_buddy_free_list(mm, &high, 0);
	assert(mm->avail == mm->size);
	gpu_buddy_fini(mm);
	module_stop();
}

static void trim_and_clear(void)
{
	module_start();
	struct gpu_buddy mm;
	assert(!gpu_buddy_init(&mm, 16 * PAGE_SIZE, PAGE_SIZE));
	LIST_HEAD(blocks);
	assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, 8 * PAGE_SIZE, PAGE_SIZE, &blocks,
		GPU_BUDDY_CONTIGUOUS_ALLOCATION | GPU_BUDDY_TRIM_DISABLE));
	struct gpu_buddy_block *block = list_first_entry(&blocks, struct gpu_buddy_block, link);
	u64 start = gpu_buddy_block_offset(block) + PAGE_SIZE;
	assert(!gpu_buddy_block_trim(&mm, &start, 3 * PAGE_SIZE, &blocks));
	assert(check_blocks(&mm, &blocks, start, start + 3 * PAGE_SIZE, true) == 3 * PAGE_SIZE);
	assert(mm.avail == mm.size - 3 * PAGE_SIZE);
	gpu_buddy_free_list(&mm, &blocks, GPU_BUDDY_CLEARED);
	assert(mm.avail == mm.size && mm.clear_avail == 3 * PAGE_SIZE);
	assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, PAGE_SIZE, PAGE_SIZE, &blocks,
		GPU_BUDDY_CLEAR_ALLOCATION));
	block = list_first_entry(&blocks, struct gpu_buddy_block, link);
	assert(gpu_buddy_block_is_clear(block) && mm.clear_avail == 2 * PAGE_SIZE);
	gpu_buddy_free_list(&mm, &blocks, GPU_BUDDY_CLEARED);
	gpu_buddy_reset_clear(&mm, true); assert(mm.clear_avail == mm.size);
	gpu_buddy_reset_clear(&mm, false); assert(!mm.clear_avail);
	assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, mm.size, PAGE_SIZE, &blocks, 0));
	assert(!mm.avail && check_blocks(&mm, &blocks, 0, mm.size, true) == mm.size);
	gpu_buddy_free_list(&mm, &blocks, 0);
	gpu_buddy_fini(&mm); module_stop();
}

/* A clear, top-down request that the clear tree cannot satisfy takes the
 * highest free block of the dirty tree, not the smallest-order one wherever
 * it lies (patches/linux/gpu-buddy-topdown-fallback.patch): amdgpu's KFD
 * VRAM BOs are VRAM_CLEARED and top-down, and a small BAR exposes only the
 * low VRAM to the CPU. */
static void topdown_clear_fallback(bool expect_top)
{
	module_start();
	struct gpu_buddy mm;
	const u64 mib = 1ULL << 20;
	assert(!gpu_buddy_init(&mm, 64 * mib, PAGE_SIZE));
	struct list_head blocks[64];
	for (int i = 0; i < 64; i++) {
		INIT_LIST_HEAD(&blocks[i]);
		assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, mib, mib, &blocks[i], 0));
	}
	/* Free, dirty: one 1 MiB block low, and the 4 MiB at the top (one block
	 * of a higher order once they coalesce). */
	for (int i = 0; i < 64; i++) {
		u64 offset = gpu_buddy_block_offset(list_first_entry(&blocks[i], struct gpu_buddy_block, link));
		if (offset == 2 * mib || offset >= 60 * mib)
			gpu_buddy_free_list(&mm, &blocks[i], 0);
	}
	assert(!mm.clear_avail && mm.avail == 5 * mib);
	LIST_HEAD(got);
	assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, mib, mib, &got,
		GPU_BUDDY_CLEAR_ALLOCATION | GPU_BUDDY_TOPDOWN_ALLOCATION));
	u64 at = gpu_buddy_block_offset(list_first_entry(&got, struct gpu_buddy_block, link));
	printf("clear top-down fallback took the block at %llu MiB\n", (unsigned long long)(at / mib));
	assert(expect_top ? at >= 60 * mib : at == 2 * mib);
	gpu_buddy_free_list(&mm, &got, 0);
	for (int i = 0; i < 64; i++)
		if (!list_empty(&blocks[i]))
			gpu_buddy_free_list(&mm, &blocks[i], 0);
	gpu_buddy_fini(&mm); module_stop();
}

static void failures(void)
{
	dext_heap_test_fail_after(0);
	assert(linuxu_module_init_gpu_buddy_module_init() == -ENOMEM);
	dext_heap_test_fail_after(-1);
	assert(!dext_heap_test_live_allocations());
	bool initialized = false;
	for (long failure = 0; failure < 24; failure++) {
		module_start();
		struct amdgpu_device device = {.gmc.real_vram_size = 32624ull * SZ_1M};
		registered = NULL;
		dext_heap_test_fail_after(failure);
		int result = amdgpu_vram_mgr_init(&device);
		dext_heap_test_fail_after(-1);
		if (!result) {
			initialized = true;
			assert(registered == &device.mman.vram_mgr.manager);
			gpu_buddy_fini(&device.mman.vram_mgr.mm);
		} else {
			assert(result == -ENOMEM && !registered && !device.mman.vram_mgr.manager.used);
		}
		module_stop();
	}
	assert(initialized);
}

static void split_failures(unsigned modes, bool report)
{
	bool allocated[4] = {0};
	for (unsigned mode = 0; mode < modes; mode++)
	for (long failure = 0; failure < 24; failure++) {
		if (report) fprintf(stderr, "range-split failure point: %ld\n", failure);
		module_start();
		struct gpu_buddy mm;
		assert(!gpu_buddy_init(&mm, 256 * PAGE_SIZE, PAGE_SIZE));
		LIST_HEAD(blocks);
		u64 begin = mode < 2 ? PAGE_SIZE : 0;
		u64 end = mode == 0 ? 2 * PAGE_SIZE : mm.size;
		u64 alignment = mode == 2 ? 2 * PAGE_SIZE : PAGE_SIZE;
		unsigned long flags = mode < 2 ? GPU_BUDDY_RANGE_ALLOCATION : 0;
		dext_heap_test_fail_after(failure);
		int result = gpu_buddy_alloc_blocks(&mm, begin, end, PAGE_SIZE,
			alignment, &blocks, flags);
		dext_heap_test_fail_after(-1);
		if (!result) {
			allocated[mode] = true;
			assert(check_blocks(&mm, &blocks, begin, end, true) == PAGE_SIZE);
			struct gpu_buddy_block *block = list_first_entry(&blocks, struct gpu_buddy_block, link);
			assert(!(gpu_buddy_block_offset(block) % alignment));
			gpu_buddy_free_list(&mm, &blocks, 0);
		} else {
			assert(result == (mode == 0 ? -ENOMEM : -ENOSPC) && list_empty(&blocks));
		}
		assert(mm.avail == mm.size);
		/* Failure must leave the original range reusable. */
		assert(!gpu_buddy_alloc_blocks(&mm, 0, mm.size, mm.size, PAGE_SIZE, &blocks, 0));
		gpu_buddy_free_list(&mm, &blocks, 0);
		gpu_buddy_fini(&mm); module_stop();
	}
	for (unsigned mode = 0; mode < modes; mode++) assert(allocated[mode]);
}

static void boundaries(void)
{
	assert(range_overflows_t(u64, UINT64_MAX - 7, 16, UINT64_MAX));
	assert(range_overflows_t(u64, 4, 0, 4));
	assert(!range_overflows_t(u64, 0, 4, 4));
	unsigned int once = 1;
	assert(!range_overflows(once++, 1u, 4u) && once == 2);
	module_start();
	struct gpu_buddy mm;
	assert(gpu_buddy_init(&mm, 0, PAGE_SIZE) == -EINVAL);
	assert(gpu_buddy_init(&mm, PAGE_SIZE, 0) == -EINVAL);
	assert(gpu_buddy_init(&mm, PAGE_SIZE, 1024) == -EINVAL);
	assert(gpu_buddy_init(&mm, 8 * PAGE_SIZE, 3 * PAGE_SIZE) == -EINVAL);
	assert(!gpu_buddy_init(&mm, 7 * PAGE_SIZE + 17, PAGE_SIZE));
	assert(mm.size == 7 * PAGE_SIZE && mm.n_roots == 3);
	LIST_HEAD(blocks);
	assert(gpu_buddy_alloc_blocks(&mm, 0, mm.size, 0, PAGE_SIZE, &blocks, 0) == -EINVAL);
	assert(gpu_buddy_alloc_blocks(&mm, 0, mm.size, PAGE_SIZE + 1, PAGE_SIZE, &blocks, 0) == -EINVAL);
	assert(gpu_buddy_alloc_blocks(&mm, mm.size, mm.size, PAGE_SIZE, PAGE_SIZE, &blocks, 0) == -EINVAL);
	assert(list_empty(&blocks) && mm.avail == mm.size);
	gpu_buddy_fini(&mm); module_stop();
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--topdown-negative-control")) {
		topdown_clear_fallback(false);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "--range-split-negative-control")) {
		negative_control = true;
		split_failures(1, true);
		return 0;
	}
	assert(argc == 1);
	boundaries(); actual_vram_init(); trim_and_clear(); topdown_clear_fallback(true); failures(); split_failures(4, false);
	puts("pinned GPU buddy: actual VRAM init, 32624MiB heap, BAR ranges, overlap rejection, trim/clear, clear top-down fallback, OOM rollback passed");
}
