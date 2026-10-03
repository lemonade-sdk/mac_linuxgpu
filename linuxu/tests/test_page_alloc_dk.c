/* Native mock of the DriverKit on-demand page backing path. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <rt/dart.h>

/* Include the production implementation to construct a saturated tombstone
 * table without adding test entry points to the shipped driver. */
#include "../src/mm/page.c"

static size_t largest_allocation;
static unsigned int backing_allocations;
static unsigned int backing_live;
static int fail_allocation, fail_completion;
static int invalid_iova;
static uint64_t next_iova = 0x200000;
static struct {
	void *cpu;
	size_t size;
	uint64_t iova;
} mappings[32];

void *dext_cpu_alloc_pages(size_t size)
{
	assert(size && !(size & (PAGE_SIZE - 1)));
	return aligned_alloc(PAGE_SIZE, size);
}
int dext_cpu_free_pages(void *cpu, size_t size)
{
	assert(cpu && size && !(size & (PAGE_SIZE - 1)));
	free(cpu);
	return 0;
}

int dext_dma_alloc_coherent(size_t size, void **cpu, uint64_t *iova)
{
	void *p = NULL;
	*cpu = NULL;
	*iova = 0;
	if (fail_allocation) return -1;
	/* A teardown/reset arriving during allocation must preserve reservations. */
	uint64_t charged = linuxu_dart_used();
	assert(charged >= size);
	linuxu_dart_reset();
	assert(linuxu_dart_used() == charged);
	if (size > largest_allocation)
		largest_allocation = size;
	if (size > 4 * PAGE_SIZE)
		return -1; /* catches accidental giant arena reservation */
	if (posix_memalign(&p, PAGE_SIZE, size)) return -1;
	unsigned int slot;
	for (slot = 0; slot < 32 && mappings[slot].cpu; slot++);
	assert(slot < 32);
	*cpu = p;
	*iova = invalid_iova ? 0 : next_iova;
	next_iova += 8 * PAGE_SIZE;
	mappings[slot].cpu = p;
	mappings[slot].size = size;
	mappings[slot].iova = *iova;
	backing_live++;
	backing_allocations++;
	return 0;
}

int dext_dma_free_coherent(void *cpu, size_t size)
{
	unsigned int slot;
	for (slot = 0; slot < 32 && mappings[slot].cpu != cpu; slot++);
	assert(cpu && slot < 32 && mappings[slot].size == size);
	if (fail_completion) return -1;
	free(cpu);
	memset(&mappings[slot], 0, sizeof(mappings[slot]));
	backing_live--;
	return 0;
}

static void check_dma_failure_cleanup(void)
{
	dma_addr_t iova, alias;
	unsigned char *cpu = linuxu_dma_alloc_coherent(NULL, 2 * PAGE_SIZE, &iova, 0);
	assert(cpu && backing_live == 1);
	alias = linuxu_dma_map_single(NULL, cpu + PAGE_SIZE, PAGE_SIZE, DMA_BIDIRECTIONAL);
	assert(alias == iova + PAGE_SIZE);
	assert(linuxu_dma_cpu_address(alias) == cpu + PAGE_SIZE);
	assert(dma_mapping_error(NULL, linuxu_dma_map_single(NULL, cpu + PAGE_SIZE,
					2 * PAGE_SIZE, DMA_BIDIRECTIONAL)));
	linuxu_dma_free_coherent(NULL, 2 * PAGE_SIZE, cpu, iova);
	assert(backing_live == 1 && linuxu_dart_used() == 2 * PAGE_SIZE);
	assert(dma_mapping_error(NULL, linuxu_dma_map_single(NULL, cpu,
					PAGE_SIZE, DMA_BIDIRECTIONAL)));
	/* An unknown subrange/direction cannot release another live alias. */
	linuxu_dma_unmap_single(NULL, iova, PAGE_SIZE, DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_single(NULL, alias, PAGE_SIZE, DMA_TO_DEVICE);
	assert(backing_live == 1);
	fail_completion = 1;
	linuxu_dma_unmap_single(NULL, alias, PAGE_SIZE, DMA_BIDIRECTIONAL);
	assert(backing_live == 1 && linuxu_dart_used() == 2 * PAGE_SIZE);
	assert(linuxu_dart_table_count() == 1);
	linuxu_dart_reset();
	assert(linuxu_dart_used() == 2 * PAGE_SIZE);
	fail_completion = 0;
	/* Retry uses the recorded allocation size, not an untrusted free length. */
	linuxu_dma_free_coherent(NULL, 1, cpu, iova);
	assert(backing_live == 0 && linuxu_dart_used() == 0);
	linuxu_dma_free_coherent(NULL, 2 * PAGE_SIZE, cpu, iova);
	linuxu_dma_unmap_single(NULL, alias, PAGE_SIZE, DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_single(NULL, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	assert(linuxu_dart_table_count() == 0);
}

static void check_stream_failure(void)
{
	unsigned char *source = malloc(16);
	assert(source);
	memset(source, 0x6a, 16);
	dma_addr_t iova = linuxu_dma_map_single(NULL, source, 16, DMA_BIDIRECTIONAL);
	assert(!dma_mapping_error(NULL, iova));
	assert(backing_live == 1);
	fail_completion = 1;
	linuxu_dma_unmap_single(NULL, iova, 16, DMA_BIDIRECTIONAL);
	assert(backing_live == 1 && linuxu_dart_used() == PAGE_SIZE);
	assert(linuxu_dart_table_count() == 1);
	free(source);
	/* The DMA buffer is quarantined; stale sync/unmap must not touch source. */
	linuxu_dma_sync_single_for_cpu(NULL, iova, 16, DMA_BIDIRECTIONAL);
	linuxu_dma_sync_single_for_device(NULL, iova, 16, DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_single(NULL, iova, 16, DMA_BIDIRECTIONAL);
	assert(linuxu_dma_cpu_address(iova) == NULL);
	linuxu_dart_reset();
	assert(linuxu_dart_used() == PAGE_SIZE && backing_live == 1);
	puts("DriverKit streaming DMA failure retains backing and rejects stale copies");
}

static void check_unpublished_failure(int stream)
{
	unsigned char source[16] = {0};
	dma_addr_t address = 0;
	invalid_iova = fail_completion = 1;
	if (stream)
		assert(dma_mapping_error(NULL, linuxu_dma_map_single(NULL, source,
			sizeof(source), DMA_BIDIRECTIONAL)));
	else
		assert(!linuxu_dma_alloc_coherent(NULL, PAGE_SIZE, &address, 0));
	assert(backing_live == 1 && linuxu_dart_used() == PAGE_SIZE);
	assert(linuxu_dart_table_count() == 1);
	linuxu_dart_reset();
	assert(linuxu_dart_used() == PAGE_SIZE);
	puts("Unpublished invalid DMA mapping retains backing and budget after failed completion");
}

static void check_stream_mask_failure(void)
{
	unsigned char source[16] = {0};
	struct device dev = {0}; /* Missing dma_mask has the Linux 32-bit default. */
	const uint64_t rejected_iova = 1ULL << 32;
	next_iova = rejected_iova;
	fail_completion = 1;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, source,
		sizeof(source), DMA_BIDIRECTIONAL)));
	assert(backing_live == 1 && linuxu_dart_used() == PAGE_SIZE);
	assert(linuxu_dart_table_count() == 1);
	/* A failed unpublished map has no client source to copy back into. */
	linuxu_dma_sync_single_for_cpu(&dev, rejected_iova, sizeof(source), DMA_BIDIRECTIONAL);
	linuxu_dma_unmap_single(&dev, rejected_iova, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!linuxu_dma_cpu_address(rejected_iova));
	linuxu_dart_reset();
	assert(backing_live == 1 && linuxu_dart_used() == PAGE_SIZE);
	puts("Streaming mask rejection retains quarantined DMA backing after failed completion");
}

static void check_stream_masks(void)
{
	unsigned char source[16] = {0};
	u64 mask = UINT32_MAX;
	struct device dev = {.dma_mask = &mask};
	uint64_t saved_iova = next_iova;
	next_iova = 1ULL << 32;
	dma_addr_t owner_dma;
	void *owner = linuxu_dma_alloc_coherent(NULL, PAGE_SIZE, &owner_dma, 0);
	assert(owner && owner_dma >= (1ULL << 32));
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, owner, 16, DMA_BIDIRECTIONAL)));
	dev.dma_mask = NULL;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, owner, 16, DMA_BIDIRECTIONAL)));
	dev.dma_mask = &mask; mask = (1ULL << 44) - 1;
	dev.bus_dma_limit = UINT32_MAX;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, owner, 16, DMA_BIDIRECTIONAL)));
	dev.bus_dma_limit = 0;
	dma_addr_t alias = linuxu_dma_map_single(&dev, owner, 16, DMA_BIDIRECTIONAL);
	assert(alias == owner_dma);
	linuxu_dma_unmap_single(&dev, alias, 16, DMA_BIDIRECTIONAL);
	linuxu_dma_free_coherent(NULL, PAGE_SIZE, owner, owner_dma);
	assert(!backing_live && !linuxu_dart_used());

	mask = UINT32_MAX;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL)));
	dev.dma_mask = NULL;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL)));
	dev.dma_mask = &mask; mask = (1ULL << 44) - 1;
	dev.bus_dma_limit = UINT32_MAX;
	assert(dma_mapping_error(&dev, linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL)));
	assert(!backing_live && !linuxu_dart_used());
	dev.bus_dma_limit = 0;
	dma_addr_t bounce = linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!dma_mapping_error(&dev, bounce));
	linuxu_dma_unmap_single(&dev, bounce, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!backing_live && !linuxu_dart_used());

	/* An aperture's final complete page is valid, including its last byte. */
	next_iova = (1ULL << 32) - PAGE_SIZE;
	dev.dma_mask = NULL;
	owner = linuxu_dma_alloc_coherent(NULL, PAGE_SIZE, &owner_dma, 0);
	assert(owner);
	alias = linuxu_dma_map_single(&dev, (char *)owner + PAGE_SIZE - 16, 16, DMA_BIDIRECTIONAL);
	assert(alias == (1ULL << 32) - 16);
	linuxu_dma_unmap_single(&dev, alias, 16, DMA_BIDIRECTIONAL);
	linuxu_dma_free_coherent(NULL, PAGE_SIZE, owner, owner_dma);
	next_iova = (1ULL << 32) - PAGE_SIZE;
	bounce = linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!dma_mapping_error(&dev, bounce));
	linuxu_dma_unmap_single(&dev, bounce, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!backing_live && !linuxu_dart_used());
	next_iova = saved_iova;
}

/* Each device's masks bound its own mappings; dart.c holds no fixed GPU
 * width. A DART IOVA at 1 TiB + 2 GiB is reachable by 44- and 64-bit
 * devices, not by 32- or 40-bit ones, and one above 2^44 only by 64-bit. */
static void check_device_mask_widths(void)
{
	static const unsigned int widths[] = {32, 40, 44, 64};
	const uint64_t saved_iova = next_iova;
	for (unsigned int i = 0; i < 4; i++) {
		const unsigned int bits = widths[i];
		u64 mask = DMA_BIT_MASK(bits);
		struct device dev = {.dma_mask = &mask, .coherent_dma_mask = DMA_BIT_MASK(bits)};
		const uint64_t placements[] = {0x200000, (1ULL << 40) + (2ULL << 30), 1ULL << 45};
		for (unsigned int j = 0; j < 3; j++) {
			const uint64_t at = placements[j];
			const int reachable = bits == 64 || at + PAGE_SIZE - 1 <= mask;
			unsigned char source[16] = {0};
			dma_addr_t dma = 0;
			next_iova = at;
			void *cpu = linuxu_dma_alloc_coherent(&dev, PAGE_SIZE, &dma, GFP_KERNEL);
			assert(!!cpu == reachable);
			if (cpu) {
				assert(dma == at);
				linuxu_dma_free_coherent(&dev, PAGE_SIZE, cpu, dma);
			}
			next_iova = at;
			dma = linuxu_dma_map_single(&dev, source, sizeof(source), DMA_BIDIRECTIONAL);
			assert(!!dma_mapping_error(&dev, dma) == !reachable);
			if (reachable)
				linuxu_dma_unmap_single(&dev, dma, sizeof(source), DMA_BIDIRECTIONAL);
			assert(!backing_live && !linuxu_dart_used());
		}
	}
	next_iova = saved_iova;
	puts("DART mappings honor 32/40/44/64-bit device masks, with no fixed GPU width");
}

int main(int argc, char **argv)
{
	struct page *block;
	struct page *again;
	unsigned char *base;
	if (argc > 1 && !strcmp(argv[1], "--stream-mask-completion-failure")) {
		check_stream_mask_failure();
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--stream-completion-failure")) {
		check_stream_failure();
		return 0;
	}
	if (argc > 1 && !strncmp(argv[1], "--unpublished-", 14)) {
		check_unpublished_failure(!strcmp(argv[1], "--unpublished-stream"));
		return 0;
	}
	assert(linuxu_page_pool_extend(16) == 0);
	dma_addr_t oversized = 0;
	assert(!linuxu_dma_alloc_coherent(NULL, linuxu_dart_budget() + PAGE_SIZE,
		&oversized, 0));
	assert(backing_allocations == 0 && linuxu_dart_used() == 0);
	fail_allocation = 1;
	assert(alloc_pages(GFP_KERNEL, 2) == NULL);
	assert(backing_live == 0 && linuxu_dart_used() == 0);
	fail_allocation = 0;

	block = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
	assert(block && backing_allocations == 1);
	assert(largest_allocation == 4 * PAGE_SIZE);
	base = page_address(block);
	assert(base && ((uintptr_t)base & (PAGE_SIZE - 1)) == 0);
	for (unsigned int i = 0; i < 4; i++) {
		assert(page_address(&block[i]) == base + i * PAGE_SIZE);
		assert(virt_to_page_internal(base + i * PAGE_SIZE + 17) == &block[i]);
		assert(pfn_to_page(page_to_pfn(&block[i])) == &block[i]);
	}
	for (unsigned int i = 0; i < 4 * PAGE_SIZE; i++)
		assert(base[i] == 0);
	memset(base, 0xa5, 4 * PAGE_SIZE);
	__free_pages(block + 1, 0); /* a tail page does not own the block */
	__free_pages(block, 1); /* wrong order must not release the backing */
	free_pages((unsigned long)base, 128); /* invalid shift must not execute */
	assert(page_address(block) == base && backing_live == 1);
	__free_pages(block, 2);
	assert(virt_to_page_internal(base) == NULL);
	assert(page_address(block) == NULL);
	__free_pages(block, 2); /* double free is harmless */

	again = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
	assert(again == block && backing_allocations == 2);
	base = page_address(again);
	for (unsigned int i = 0; i < 4 * PAGE_SIZE; i++)
		assert(base[i] == 0);
	assert(virt_to_page_internal(base + PAGE_SIZE + 1) == &again[1]);
	__free_pages(again, 2);
	assert(backing_live == 0 && linuxu_dart_used() == 0);
	check_dma_failure_cleanup();
	block = alloc_page(0);
	assert(block && backing_live == 1);
	assert(get_page(block) && page_ref_count(block) == 2);
	put_page(block);
	assert(backing_live == 1 && page_address(block));
	put_page(block);
	assert(!backing_live && !page_address(block) && !linuxu_dart_used());
	assert(!get_page(block));

	/* Linux sg_set_page's length/offset ordering must reach the real DART
	 * alias path, including lists joined by a chain marker. */
	block = alloc_pages(GFP_KERNEL, 1);
	assert(block);
	struct scatterlist first[2], second[1];
	sg_init_table(first, 2);
	sg_init_table(second, 1);
	sg_set_page(first, block, PAGE_SIZE - 7, 7);
	sg_set_page(second, block + 1, 13, 11);
	sg_chain(first, 2, second);
	assert(linuxu_dma_map_sg(NULL, first, 2, DMA_BIDIRECTIONAL) == 2);
	assert(linuxu_dma_cpu_address(first[0].dma_address) == (char *)page_address(block) + 7);
	assert(linuxu_dma_cpu_address(second[0].dma_address) == (char *)page_address(block + 1) + 11);
	assert(linuxu_dart_used() == 2 * PAGE_SIZE); /* aliases do not allocate bounce buffers */
	linuxu_dma_unmap_sg(NULL, first, 2, DMA_BIDIRECTIONAL);
	__free_pages(block, 1);
	assert(backing_live == 0 && linuxu_dart_used() == 0);

	/* A freed page plus nonzero offset must not become a low CPU pointer. */
	struct scatterlist sg[2] = {0};
	block = alloc_pages(GFP_KERNEL, 0);
	assert(block);
	sg[0].page = block;
	sg[0].length = PAGE_SIZE;
	sg[1].page = pfn_to_page(1);
	sg[1].offset = 8;
	sg[1].length = 16;
	assert(linuxu_dma_map_sg(NULL, sg, 2, DMA_BIDIRECTIONAL) == 0);
	__free_pages(block, 0);
	assert(backing_live == 0 && linuxu_dart_used() == 0);
	assert(linuxu_dart_table_count() == 0);
	/* Compound tails retain one shared allocation through their head. */
	block = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	assert(block && backing_live == 1);
	assert(compound_head(block + 3) == block && folio_nr_pages(page_folio(block + 3)) == 4);
	assert(get_page(block + 3) && page_ref_count(block) == 2);
	put_page(block);
	assert(backing_live == 1 && page_address(block + 3));
	put_page(block + 3);
	assert(!backing_live && !page_address(block) && !linuxu_dart_used());

	/* Split pages have independent references, with shared DMA ownership
	 * retained until the final slice is released. Retired slots stay reserved. */
	block = alloc_pages(GFP_KERNEL, 2);
	assert(block);
	split_page(block, 2);
	put_page(block);
	assert(!page_address(block) && backing_live == 1);
	again = alloc_page(GFP_KERNEL);
	assert(again && again != block && again > block + 3);
	put_page(again);
	put_page(block + 2);
	put_page(block + 1);
	assert(backing_live == 1 && page_address(block + 3));
	put_page(block + 3);
	assert(!backing_live && !linuxu_dart_used());

	/* The unchanged TTM DMA allocation path writes private metadata through
	 * virt_to_page(), which must resolve to a live, aligned descriptor. */
	dma_addr_t attrs_dma;
	base = dma_alloc_attrs(NULL, 3 * PAGE_SIZE, &attrs_dma, GFP_KERNEL,
		DMA_ATTR_FORCE_CONTIGUOUS);
	assert(base && backing_live == 1 && linuxu_dart_used() == 4 * PAGE_SIZE);
	block = virt_to_page(base);
	assert(block && page_address(block) == base && block->_pad1 == 2);
	block->private = 0x1234;
	assert(linuxu_dma_cpu_address(attrs_dma) == base);
	dma_free_attrs(NULL, PAGE_SIZE, base, attrs_dma, 0);
	dma_free_attrs(NULL, 3 * PAGE_SIZE, base, attrs_dma + PAGE_SIZE, 0);
	assert(backing_live == 1 && block->private == 0x1234);
	dma_free_attrs(NULL, 3 * PAGE_SIZE, base, attrs_dma, 0);
	assert(!backing_live && !page_address(block) && !linuxu_dart_used());
	block = linuxu_alloc_cpu_page(GFP_KERNEL | __GFP_ZERO);
	assert(block && !backing_live && !linuxu_dart_used());
	assert(page_address(block) && virt_to_page(page_address(block)) == block);
	put_page(block);
	/* A valid 44-bit DART address may still exceed the caller's narrower
	 * aperture. Both coherent entry points must reject and release it. */
	struct device limited = {.coherent_dma_mask = UINT32_MAX};
	uint64_t prior_iova = next_iova;
	next_iova = 1ULL << 32;
	assert(!dma_alloc_attrs(&limited, PAGE_SIZE, &attrs_dma, GFP_KERNEL, 0));
	assert(attrs_dma == DMA_MAPPING_ERROR && !backing_live && !linuxu_dart_used());
	assert(!linuxu_dma_alloc_coherent(&limited, PAGE_SIZE, &attrs_dma, GFP_KERNEL));
	assert(!linuxu_dma_alloc_coherent(NULL, PAGE_SIZE, &attrs_dma, GFP_KERNEL | __GFP_DMA32));
	assert(!alloc_pages(GFP_KERNEL | __GFP_DMA32, 0));
	assert(!backing_live && !linuxu_dart_used());
	next_iova = prior_iova;
	limited.coherent_dma_mask = (1ULL << 44) - 1;
	limited.bus_dma_limit = PAGE_SIZE - 1;
	assert(!dma_alloc_attrs(&limited, PAGE_SIZE, &attrs_dma, GFP_KERNEL, 0));
	assert(!linuxu_dma_alloc_coherent(&limited, PAGE_SIZE, &attrs_dma, GFP_KERNEL));
	assert(!backing_live && !linuxu_dart_used());
	check_stream_masks();
	check_device_mask_widths();
	/* Churn can leave every lookup slot as a tombstone. Missing addresses
	 * must terminate instead of hanging the driver's dispatch queue. */
	for (unsigned long i = 0; i < LINUXU_VA_SLOTS; i++)
		va_slots[i].base = 1;
	alarm(5);
	assert(virt_to_page_internal((void *)0x40000) == NULL);
	va_remove(0x40000);
	va_insert(0x40000, 0);
	assert(va_lookup(0x40001) == pfn_to_page(0));
	va_remove(0x40000);
	assert(va_lookup(0x40001) == NULL);
	alarm(0);
	puts("DriverKit pages and DMA: bounds, aliases, allocation/completion failures passed");
	return 0;
}
