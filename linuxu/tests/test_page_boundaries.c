/* Native page backend bounds. Production backing teardown is exercised by
 * test_page_alloc_dk; this process only allocates a small simulated arena. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <rt/dart.h>

int main(void)
{
	assert(linuxu_page_pool_extend(SIZE_MAX / PAGE_SIZE + 1) < 0);
	assert(linuxu_page_pool_extend(16) == 0);
	assert(!page_address(NULL) && !virt_to_page_internal(NULL));
	struct page foreign = {0};
	assert(!page_address(&foreign) && page_to_pfn(&foreign) == ~0UL);
	__free_pages(&foreign, 0);
	struct page *block = alloc_pages(GFP_KERNEL, 2);
	assert(block);
	void *address = page_address(block);
	assert(address && page_to_pfn(block) % 4 == 0);
	__free_pages(block + 1, 0);
	__free_pages(block, 1);
	__free_pages(block, 63);
	__free_pages((struct page *)((char *)block + 1), 0);
	free_pages((unsigned long)address + 1, 2);
	assert(page_address(block) == address && page_ref_count(block) == 1);
	for (unsigned int i = 0; i < 4; i++) assert(page_ref_count(block + i) == 1);
	__free_pages(block, 2);
	assert(!page_address(block) && !virt_to_page_internal(address));
	__free_pages(block, 2);
	struct page *again = alloc_pages(GFP_KERNEL, 2);
	assert(again == block);
	__free_pages(again, 2);
	assert(get_order(1) == 0 && get_order(PAGE_SIZE) == 0);
	assert(get_order(PAGE_SIZE + 1) == 1 && get_order(2 * PAGE_SIZE) == 1);
	assert(get_order(2UL << 20) == 7);
	assert(get_order(1UL << 40) == 40 - PAGE_SHIFT);
	assert(get_order(ULONG_MAX) == BITS_PER_LONG - PAGE_SHIFT);
	assert(get_order(0) == BITS_PER_LONG - PAGE_SHIFT);
	struct page *source = alloc_page(0), *dest = alloc_page(0);
	assert(source && dest);
	memset(page_address(source), 0x5a, PAGE_SIZE);
	copy_highpage(dest, source);
	assert(!memcmp(page_address(dest), page_address(source), PAGE_SIZE));
	assert(get_page(source) && page_ref_count(source) == 2);
	put_page(source);
	assert(page_ref_count(source) == 1 && page_address(source));
	put_page(source);
	assert(!page_address(source) && !get_page(source));
	put_page(dest);
	/* The native backend must preserve every alias, even when addresses hash
	 * to the same bucket, and cannot refund unknown or mismatched unmaps. */
	source = alloc_page(0);
	dma_addr_t aliases[1100];
	for (unsigned i = 0; i < 1100; i++) {
		aliases[i] = linuxu_dma_map_page(NULL, source, 7, 13, DMA_BIDIRECTIONAL);
		assert(!dma_mapping_error(NULL, aliases[i]));
	}
	assert(linuxu_dart_used() == 1100 * 20 && linuxu_dart_table_count() == 1100);
	linuxu_dart_reset();
	linuxu_dma_unmap_page(NULL, aliases[0], 12, DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_page(NULL, aliases[0], 13, DMA_TO_DEVICE);
	linuxu_dma_unmap_page(NULL, aliases[0] + 1, 13, DMA_BIDIRECTIONAL);
	assert(linuxu_dart_used() == 1100 * 20 && linuxu_dart_table_count() == 1100);
	for (unsigned i = 0; i < 1100; i++) linuxu_dma_unmap_page(NULL, aliases[i], 13, DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_page(NULL, aliases[0], 13, DMA_BIDIRECTIONAL);
	assert(!linuxu_dart_used() && !linuxu_dart_table_count());
	assert(dma_mapping_error(NULL, linuxu_dma_map_page(NULL, source, 1, SIZE_MAX, DMA_BIDIRECTIONAL)));
	put_page(source);
	dma_addr_t coherent_dma;
	void *coherent = linuxu_dma_alloc_coherent(NULL, PAGE_SIZE, &coherent_dma, 0);
	assert(coherent);
	linuxu_dma_free_coherent(NULL, PAGE_SIZE, coherent, coherent_dma + 1);
	assert(linuxu_dart_used() == PAGE_SIZE);
	linuxu_dma_free_coherent(NULL, 1, coherent, coherent_dma);
	linuxu_dma_free_coherent(NULL, PAGE_SIZE, coherent, coherent_dma);
	linuxu_dma_free_coherent(NULL, SIZE_MAX, (void *)0x10000, 0x10000);
	assert(!linuxu_dart_used() && !linuxu_dart_table_count());
	struct vm_area_struct vma = {.vm_start = 2 * PAGE_SIZE, .vm_end = PAGE_SIZE};
	assert(vma_pages(&vma) == 0 && vma_pages(NULL) == 0);
	puts("native pages: overflow, foreign/interior descriptors, wrong-order/tail frees and stale VAs passed");
}
