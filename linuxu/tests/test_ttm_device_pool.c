/* Strict-link smoke test of the pinned upstream TTM device and page pool. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "mock_memory_sysctl.h"

#include <linux/device.h>
#include <linux/mm.h>
#include <linux/sysinfo.h>
#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_pool.h>
#include <drm/ttm/ttm_tt.h>
#include <rt/dart.h>

int main(void)
{
	struct ttm_device bdev = {0};
	const struct ttm_device_funcs funcs = {0};
	struct drm_vma_offset_manager vma;
	struct address_space mapping = {0};
	struct device dev = {0};
	struct ttm_operation_ctx ctx = {0};
	struct ttm_tt tt = {0};
	struct page *pages[1] = {0};
	dma_addr_t dma[1] = {0};
	struct page *prior;
	int ret;

	_Static_assert(PAGE_SIZE == 16384, "TTM must use host 16 KiB pages");
	assert(linuxu_sysinfo_init() == 0);
	linuxu_dart_reset();
	prior = alloc_page(GFP_KERNEL);
	assert(prior);
	memset(page_address(prior), 0xa5, PAGE_SIZE);
	__free_page(prior);

	device_initialize(&dev);
	drm_vma_offset_manager_init(&vma, 0, 1024);
	ret = ttm_device_init(&bdev, &funcs, &dev, &mapping, &vma, 0);
	assert(ret == 0);
	assert(ttm_glob.dummy_read_page);
	assert(page_address(ttm_glob.dummy_read_page));
	for (unsigned int i = 0; i < PAGE_SIZE; i++)
		assert(((unsigned char *)page_address(ttm_glob.dummy_read_page))[i] == 0);

	tt.pages = pages;
	tt.dma_address = dma;
	tt.num_pages = 1;
	tt.caching = ttm_cached;
	tt.page_flags = TTM_TT_FLAG_ZERO_ALLOC;
	ret = ttm_pool_alloc(&bdev.pool, &tt, &ctx);
	assert(ret == 0);
	assert(tt.pages[0]);
	assert(tt.dma_address[0]);
	assert((void *)(uintptr_t)tt.dma_address[0] == page_address(tt.pages[0]));
	assert(linuxu_dart_used() == PAGE_SIZE);
	assert(linuxu_dart_table_count() == 1);
	for (unsigned int i = 0; i < PAGE_SIZE; i++)
		assert(((unsigned char *)page_address(tt.pages[0]))[i] == 0);
	memset(page_address(tt.pages[0]), 0x5a, PAGE_SIZE);
	ttm_pool_free(&bdev.pool, &tt);
	assert(linuxu_dart_used() == 0);
	assert(linuxu_dart_table_count() == 0);
	ttm_device_fini(&bdev);
	drm_vma_offset_manager_destroy(&vma);
	put_device(&dev);
	(void)write(1, "upstream TTM device and 16 KiB pool passed\n", 43);
	return 0;
}
