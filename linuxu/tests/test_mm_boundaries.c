/* Offline MM boundary checks. vmap uses its DriverKit code and heap adapter;
 * DMA/CPU aliases are checked in-memory mocks, never IOKit or a GPU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/hmm.h>
#include <linux/memremap.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/shrinker.h>
#include <linux/io-mapping.h>
#include "dext_heap_backend.h"

extern int linuxu_hmm_register_range(unsigned long, unsigned long);
extern struct resource iomem_resource;
extern struct resource *devm_request_free_mem_region(struct device *, struct resource *, resource_size_t);

static struct page pages[3];
static void *addresses[3];
static struct { dma_addr_t dma; void *cpu; } pins[4];
static unsigned int live_pins, attempts, alias_live;
static int fail_pin, fail_alias;
static unsigned int io_live;
static int fail_io;
static unsigned char io_window[2 * PAGE_SIZE];

void *rt_ioremap_active(uint64_t phys, uint64_t size)
{
	assert(phys == PAGE_SIZE && size == sizeof(io_window));
	if (fail_io) return NULL;
	io_live++;
	return io_window;
}
void rt_mmio_free(void *address)
{
	assert(address == io_window && io_live);
	io_live--;
}

bool linuxu_get_page(struct page *page)
{
	if (!page || !page_address(page)) return false;
	if (++attempts == fail_pin) return false;
	atomic_inc(&page->refcount);
	live_pins++;
	return true;
}
void linuxu_put_page(struct page *page)
{
	assert(page && live_pins && page_ref_count(page) > 1);
	atomic_dec(&page->refcount);
	live_pins--;
}

unsigned long page_to_pfn(const struct page *page)
{
	for (unsigned long i = 0; i < 3; i++) if (page == &pages[i]) return i;
	return ULONG_MAX;
}

void *page_address(const struct page *page)
{
	for (unsigned int i = 0; i < 3; i++) if (page == &pages[i]) return addresses[i];
	return NULL;
}
dma_addr_t linuxu_dma_map_single(struct device *dev, void *cpu, size_t size, enum dma_data_direction direction)
{
	/* CPU aliases must not pin DART mappings or create bounce copies. */
	abort();
}
void *linuxu_dma_cpu_address(dma_addr_t address)
{
	for (unsigned int i = 0; i < 4; i++) if (pins[i].dma == address) return pins[i].cpu;
	return NULL;
}
void linuxu_dma_unmap_single(struct device *dev, dma_addr_t address, size_t size, enum dma_data_direction direction)
{
	(void)dev; assert(size == PAGE_SIZE && direction == DMA_BIDIRECTIONAL);
	for (unsigned int i = 0; i < 4; i++) if (pins[i].dma == address && pins[i].cpu) {
		memset(&pins[i], 0, sizeof(pins[i])); live_pins--; return;
	}
	abort();
}
void *dext_dma_vmap_pages(const void *const *cpu, size_t count)
{
	assert(count == 2 && cpu[0] && cpu[1] && live_pins == 2);
	if (fail_alias) return NULL;
	void *alias = aligned_alloc(PAGE_SIZE, count * PAGE_SIZE);
	if (alias) alias_live++;
	return alias;
}
void dext_dma_vunmap_pages(const void *alias)
{
	assert(alias && alias_live); alias_live--; free((void *)alias);
}

static void check_vmap(void)
{
	void *backing = aligned_alloc(PAGE_SIZE, 3 * PAGE_SIZE);
	assert(backing);
	for (unsigned int i = 0; i < 3; i++) addresses[i] = (char *)backing + i * PAGE_SIZE;
	for (unsigned int i = 0; i < 3; i++) atomic_set(&pages[i].refcount, 1);
	struct page *contiguous[] = {&pages[0], &pages[1], &pages[2]};
	assert(vmap(contiguous, 3, 0, 0) == backing);
	assert(live_pins == 3 && vmalloc_to_page((char *)backing + PAGE_SIZE + 7) == &pages[1]);
	assert(is_vmalloc_addr(backing) && !vmalloc_to_page((char *)backing + 3 * PAGE_SIZE));
	vunmap(backing);
	assert(!is_vmalloc_addr(backing));
	assert(!live_pins && !alias_live);
	struct page *scattered[] = {&pages[0], &pages[2]};
	size_t baseline = dext_heap_test_live_allocations();
	for (long failure = -1; failure < 5; failure++) {
		attempts = 0;
		dext_heap_test_fail_after(failure);
		void *alias = vmap(scattered, 2, 0, 0);
		dext_heap_test_fail_after(-1);
		if (alias) {
			assert(live_pins == 2 && alias_live == 1);
			assert(vmalloc_to_page((char *)alias + PAGE_SIZE + 3) == &pages[2]);
			vunmap(alias); vunmap(alias);
		}
		assert(!live_pins && !alias_live);
		assert(dext_heap_test_live_allocations() == baseline);
	}
	for (unsigned int mode = 0; mode < 3; mode++) {
		attempts = 0; fail_pin = mode < 2 ? mode + 1 : 0;
		fail_alias = mode == 2;
		assert(!vmap(scattered, 2, 0, 0));
		assert(!live_pins && !alias_live);
		assert(dext_heap_test_live_allocations() == baseline);
	}
	fail_pin = fail_alias = 0;
	struct page *missing[] = {&pages[0], NULL};
	assert(!vmap(missing, 2, 0, 0) && !live_pins);
	assert(!vmap(NULL, 2, 0, 0) && !vmap(scattered, 0, 0, 0));
	free(backing);
}

static unsigned long reclaim_count(struct shrinker *s, struct shrink_control *c)
{ (void)s; (void)c; return 0; }
static void check_io_mapping(void)
{
	size_t baseline = dext_heap_test_live_allocations();
	assert(kzalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR);
	assert(!io_mapping_create_wc(0, 0));
	assert(!io_mapping_create_wc(PAGE_SIZE, 0));
	assert(!io_mapping_create_wc(UINT64_MAX, 2));
	assert(dext_heap_test_live_allocations() == baseline);
	for (long failure = 0; failure < 2; failure++) {
		dext_heap_test_fail_after(failure);
		struct io_mapping *mapping = io_mapping_create_wc(PAGE_SIZE, sizeof(io_window));
		dext_heap_test_fail_after(-1);
		assert(!!mapping == (failure == 1));
		if (mapping) {
			assert(mapping->base == PAGE_SIZE && mapping->size == sizeof(io_window));
			assert(mapping->iomem == io_window && io_live == 1);
			assert(io_mapping_map_wc(mapping, 1, 7) == io_window + 1);
			assert(io_mapping_map_local_wc(mapping, PAGE_SIZE) == io_window + PAGE_SIZE);
			assert(!io_mapping_map_wc(mapping, sizeof(io_window), 1));
			assert(!io_mapping_map_wc(mapping, sizeof(io_window) - 1, 2));
			assert(!io_mapping_map_wc(mapping, 1, ULONG_MAX));
			assert(!io_mapping_map_local_wc(mapping, 1));
			assert(!io_mapping_map_local_wc(mapping, sizeof(io_window)));
			io_mapping_fini(mapping);
			assert(!io_live && !mapping->iomem);
			io_mapping_free(mapping);
		}
		assert(dext_heap_test_live_allocations() == baseline);
	}
	fail_io = 1;
	assert(!io_mapping_create_wc(PAGE_SIZE, sizeof(io_window)));
	assert(!io_live && dext_heap_test_live_allocations() == baseline);
	fail_io = 0;
	struct io_mapping stack = {0};
	assert(io_mapping_init_wc(&stack, PAGE_SIZE, sizeof(io_window)) == &stack);
	assert(io_live == 1 && stack.iomem == io_window);
	io_mapping_fini(&stack);
	assert(!io_live);
	io_mapping_free(NULL);
}
static void check_mm_contracts(void)
{
	unsigned long output[2] = {0x12345678, 0xabcdef};
	struct hmm_range range = {.start = PAGE_SIZE, .end = 2 * PAGE_SIZE, .hmm_pfns = output};
	assert(hmm_range_fault(&range) == -EOPNOTSUPP);
	assert(linuxu_hmm_register_range(range.start, range.end) == -EOPNOTSUPP);
	range.end = 0; assert(hmm_range_fault(&range) == -EINVAL);
	range.start = 1; range.end = PAGE_SIZE; assert(hmm_range_fault(&range) == -EINVAL);
	assert(hmm_range_fault(NULL) == -EINVAL);
	assert(output[0] == 0x12345678 && output[1] == 0xabcdef);
	struct dev_pagemap pgmap = {0};
	struct device dev = {0};
	assert(PTR_ERR(memremap_pages(&pgmap, 0)) == -EOPNOTSUPP);
	assert(PTR_ERR(devm_memremap_pages(&dev, &pgmap)) == -EOPNOTSUPP);
	assert(PTR_ERR(devm_request_free_mem_region(&dev, &iomem_resource, PAGE_SIZE)) == -EOPNOTSUPP);
	struct page ttm_page = {.private = 3};
	assert(!is_zone_device_page(&ttm_page) && !page_pgmap(&ttm_page));
	/* Permission changes must not clear sharing or reserve-accounting bits.
	 * amdgpu_gem.c uses VM_ACCESS_FLAGS to distinguish PROT_NONE mappings. */
	struct vm_area_struct shared = {
		.vm_flags = VM_READ | VM_SHARED | VM_MAYWRITE | VM_NORESERVE,
	};
	vm_flags_clear(&shared, VM_MAYWRITE);
	assert(shared.vm_flags == (VM_READ | VM_SHARED | VM_NORESERVE));
	assert(!(shared.vm_flags & VM_MAYSHARE));
	assert((shared.vm_flags & VM_ACCESS_FLAGS) == VM_READ);
	assert(!((VM_IO | VM_PFNMAP | VM_LOCKED) & VM_ACCESS_FLAGS));
	assert(vma_flags_to_legacy(mk_vma_flags(VMA_NORESERVE_BIT)) == VM_NORESERVE);
	assert(vma_flags_to_legacy(mk_vma_flags(0, 3, VMA_NORESERVE_BIT)) ==
	       (VM_READ | VM_SHARED | VM_NORESERVE));
	assert(VM_SHARED == (1UL << 3) && VM_MAYSHARE == (1UL << 7));
	assert(VM_PFNMAP == (1UL << 10) && VM_DONTCOPY == (1UL << 17));
	assert(VM_DONTEXPAND == (1UL << 18) && VM_DONTDUMP == (1UL << 26));
	assert(VM_MIXEDMAP == (1UL << 28));
	struct vm_area_struct vma = {.vm_flags = VM_READ};
	vm_flags_set(&vma, VM_DONTCOPY | VM_DONTEXPAND);
	assert(vma.vm_flags == (VM_READ | VM_DONTCOPY | VM_DONTEXPAND));
	/* PFN mappings are recorded in the VMA (mm.c), only inside it. */
	assert(remap_pfn_range(&vma, PAGE_SIZE, 1, PAGE_SIZE, 0) == -EINVAL);
	vma.vm_start = PAGE_SIZE;
	vma.vm_end = 2 * PAGE_SIZE;
	assert(io_remap_pfn_range(&vma, PAGE_SIZE, 1, PAGE_SIZE, 0) == 0);
	assert(vma.linuxu_pfn == 1 && vma.linuxu_pfn_bytes == PAGE_SIZE &&
	       (vma.vm_flags & (VM_IO | VM_PFNMAP)) == (VM_IO | VM_PFNMAP));
	vma.vm_flags = VM_READ | VM_DONTCOPY | VM_DONTEXPAND;
	assert(vmf_insert_pfn(&vma, PAGE_SIZE, 1) & VM_FAULT_ERROR);
	assert(vmf_insert_pfn_prot(&vma, PAGE_SIZE, 1, 0) & VM_FAULT_ERROR);
	assert(vmf_insert_page(&vma, PAGE_SIZE, &ttm_page) & VM_FAULT_ERROR);
	assert(vmf_insert_mixed(&vma, PAGE_SIZE, 1, false) & VM_FAULT_ERROR);
	assert((GFP_KERNEL & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS));
	assert(!(GFP_KERNEL & __GFP_MOVABLE));
	assert((GFP_NOFS & __GFP_IO) && !(GFP_NOFS & __GFP_FS));
	assert(!(GFP_NOIO & (__GFP_IO | __GFP_FS)));
	assert(gfpflags_allow_blocking(GFP_KERNEL));
	assert(!gfpflags_allow_blocking(GFP_ATOMIC) && !gfpflags_allow_blocking(GFP_NOWAIT));
	/* mm_users counts address-space users and the last mmput tears the
	 * address space down; mm_count keeps the structure until mmdrop. */
	struct mm_struct *mm = linuxu_mm_alloc();
	assert(mm && atomic_read(&mm->mm_users) == 1 && atomic_read(&mm->mm_count) == 1);
	assert(mmget_not_zero(mm) && atomic_read(&mm->mm_users) == 2);
	mmput(mm);
	mmgrab(mm);
	mmput(mm);
	assert(atomic_read(&mm->mm_users) == 0 && atomic_read(&mm->mm_count) == 1);
	assert(!mmget_not_zero(mm) && !mmget_not_zero(NULL));
	mmdrop(mm);
	dext_heap_test_fail_after(0); assert(!shrinker_alloc(0, "oom"));
	dext_heap_test_fail_after(-1);
	struct shrinker *shrink = shrinker_alloc(0, "ttm");
	assert(shrink && shrinker_register(shrink) == -EINVAL);
	shrink->count_objects = shrink->scan_objects = reclaim_count;
	assert(shrinker_register(shrink) == 0 && shrinker_register(shrink) == -EALREADY);
	shrinker_unregister(shrink); shrinker_unregister(shrink);
	assert(shrinker_register(shrink) == 0);
	shrinker_free(shrink);
}
int main(void)
{
	size_t baseline = dext_heap_test_live_allocations();
	check_vmap(); check_mm_contracts(); check_io_mapping();
	assert(dext_heap_test_live_allocations() == baseline);
	puts("MM boundaries: DK vmap unwind, unsupported host mappings, retained BAR I/O leases/bounds, mm holds, shrinkers and allocation flags passed");
}
