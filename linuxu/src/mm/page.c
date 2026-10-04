/* linuxu shim: stable struct page IDs and 16 KiB backing. Native tests
 * back each chunk of descriptors with a reserved host arena; DriverKit
 * allocates backing only for live blocks. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#ifdef LINUXU_DEXT_DK
#include <rt/dext_dma.h>
extern int IOSysCtlByName(const char *, void *, size_t *, void *, size_t);
#else
#include <sys/sysctl.h>
#endif

/* 16 KB page contract */
#define LINUXU_PAGE_SHIFT 14
#define LINUXU_PAGE_SIZE  (1UL << LINUXU_PAGE_SHIFT)

/* Page descriptors come in chunks, allocated as pages are needed, for at
 * most the machine's RAM in pages (hw.memsize): no fixed arena caps how
 * many pages the driver holds. What else bounds them is the platform's: in
 * the dext every page but the CPU-only ones is DMA-mapped, so the DART's
 * refusal (logged by the seam) comes first for those. A chunk never moves,
 * so a struct page pointer stays valid; the chunk array is sized once, for
 * the maximum, so lockless readers index it safely. A block of pages lies
 * within one chunk (a chunk holds far more than MAX_PAGE_ORDER pages). */
#define PAGE_CHUNK_SHIFT 14
#define PAGE_CHUNK_PAGES (1UL << PAGE_CHUNK_SHIFT)

struct linuxu_page_allocation {
	unsigned long first, count, live;
	unsigned int order;
	bool compound, split, cpu_only, attrs;
	void *cpu;
	dma_addr_t dma;
	size_t requested;
	struct device *device;
};
struct page_chunk {
	unsigned long first, pages;	/* pfn of pages[0], how many */
	struct page *page;
	signed char *order;
	struct linuxu_page_allocation **allocation;
#ifdef LINUXU_DEXT_DK
	void **backing;
	dma_addr_t *dma;
#else
	unsigned char *arena;
#endif
};
static struct page_chunk **page_chunks;	/* page_chunk_slots entries */
static unsigned long page_chunk_slots;
static unsigned long page_chunk_count;	/* published with release */
static unsigned long page_max;		/* pages at most: RAM, or the test hook's */
static pthread_cond_t page_changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
static int page_pool_ready;

static struct page_chunk *page_chunk_of(unsigned long pfn)
{
	const unsigned long c = pfn >> PAGE_CHUNK_SHIFT;

	if (c >= __atomic_load_n(&page_chunk_count, __ATOMIC_ACQUIRE))
		return NULL;
	struct page_chunk *chunk = __atomic_load_n(&page_chunks[c], __ATOMIC_ACQUIRE);
	return chunk && pfn - chunk->first < chunk->pages ? chunk : NULL;
}
/* Per-pfn fields; the pfn must lie in a published chunk. */
#define PG(pfn)		(&page_chunks[(pfn) >> PAGE_CHUNK_SHIFT]->page[(pfn) & (PAGE_CHUNK_PAGES - 1)])
#define PG_ORDER(pfn)	(page_chunks[(pfn) >> PAGE_CHUNK_SHIFT]->order[(pfn) & (PAGE_CHUNK_PAGES - 1)])
#define PG_ALLOC(pfn)	(page_chunks[(pfn) >> PAGE_CHUNK_SHIFT]->allocation[(pfn) & (PAGE_CHUNK_PAGES - 1)])
#ifdef LINUXU_DEXT_DK
#define PG_BACKING(pfn)	(page_chunks[(pfn) >> PAGE_CHUNK_SHIFT]->backing[(pfn) & (PAGE_CHUNK_PAGES - 1)])
#define PG_DMA(pfn)	(page_chunks[(pfn) >> PAGE_CHUNK_SHIFT]->dma[(pfn) & (PAGE_CHUNK_PAGES - 1)])
#endif

/* The pages the pool may hold: RAM in 16 KiB pages. */
static unsigned long page_ram_pages(void)
{
	uint64_t bytes = 0;
	size_t length = sizeof(bytes);
#ifdef LINUXU_DEXT_DK
	const int error = IOSysCtlByName("hw.memsize", &bytes, &length, NULL, 0);
#else
	const int error = sysctlbyname("hw.memsize", &bytes, &length, NULL, 0);
#endif
	if (error || length != sizeof(bytes) || bytes < LINUXU_PAGE_SIZE) {
		fprintf(stderr, "linuxu: page pool: the machine's memory size (hw.memsize) is unreadable (%d); no pages can be allocated\n",
			error);
		return 0;
	}
	return (unsigned long)(bytes >> LINUXU_PAGE_SHIFT);
}

/* Test hook: before the first allocation, hold the pool to @pages (tests
 * keep their memory small this way). Later, only a cap no larger than the
 * one in force is accepted. */
int linuxu_page_pool_extend(unsigned long pages)
{
	int r = 0;

	if (!pages || pages > SIZE_MAX / LINUXU_PAGE_SIZE ||
	    pages > SIZE_MAX / sizeof(struct page))
		return -1;
	pthread_mutex_lock(&page_lock);
	if (page_pool_ready)
		r = pages <= page_max ? 0 : -1;
	else
		page_max = pages;
	pthread_mutex_unlock(&page_lock);
	return r;
}

#ifdef LINUXU_DEXT_DK
/* Open-addressed lookup for the 16 KiB-aligned pages in live allocations,
 * twice as many slots as pages the chunks hold (rebuilt as chunks are
 * added). Empty=0 and tombstone=1; all real aligned VAs are greater than
 * one. Caller holds page_lock. */
struct linuxu_va_slot {
	uintptr_t base;
	unsigned long pfn;
};
static struct linuxu_va_slot *va_slots;
static unsigned long va_slot_count;	/* a power of two */

static unsigned long va_hash(uintptr_t base)
{
	return ((base >> LINUXU_PAGE_SHIFT) * 11400714819323198485ull) & (va_slot_count - 1);
}

static void va_insert(uintptr_t base, unsigned long pfn)
{
	unsigned long pos = va_hash(base);
	while (va_slots[pos].base > 1)
		pos = (pos + 1) & (va_slot_count - 1);
	va_slots[pos] = (struct linuxu_va_slot){ base, pfn };
}

static void va_remove(uintptr_t base)
{
	if (!va_slot_count)
		return;
	unsigned long pos = va_hash(base);
	for (unsigned long scanned = 0; scanned < va_slot_count &&
	     va_slots[pos].base; scanned++) {
		if (va_slots[pos].base == base) {
			va_slots[pos].base = 1;
			return;
		}
		pos = (pos + 1) & (va_slot_count - 1);
	}
}

static struct page *va_lookup(uintptr_t addr)
{
	uintptr_t base = addr & ~(uintptr_t)(LINUXU_PAGE_SIZE - 1);
	if (!va_slot_count)
		return NULL;
	unsigned long pos = va_hash(base);
	/* Freed addresses leave tombstones. After enough distinct allocations
	 * there may be no empty slot, even though no live page matches. */
	for (unsigned long scanned = 0; scanned < va_slot_count &&
	     va_slots[pos].base; scanned++) {
		if (va_slots[pos].base == base)
			return PG(va_slots[pos].pfn);
		pos = (pos + 1) & (va_slot_count - 1);
	}
	return NULL;
}

/* Rebuild the lookup for @pages pages (the live ones, no tombstones). */
static bool va_resize(unsigned long pages)
{
	unsigned long slots = 1;
	while (slots < 2 * pages)
		slots <<= 1;
	struct linuxu_va_slot *fresh = calloc(slots, sizeof(*fresh));
	if (!fresh)
		return false;
	struct linuxu_va_slot *old = va_slots;
	const unsigned long old_count = va_slot_count;
	va_slots = fresh;
	va_slot_count = slots;
	for (unsigned long i = 0; i < old_count; i++)
		if (old[i].base > 1)
			va_insert(old[i].base, old[i].pfn);
	free(old);
	return true;
}
#endif

static void page_chunk_free(struct page_chunk *chunk)
{
	if (!chunk)
		return;
	free(chunk->page);
	free(chunk->order);
	free(chunk->allocation);
#ifdef LINUXU_DEXT_DK
	free(chunk->backing);
	free(chunk->dma);
#else
	free(chunk->arena);
#endif
	free(chunk);
}

/* Add the next chunk of descriptors (fewer pages when the cap is near).
 * Caller holds page_lock. False when the cap is reached or memory is out. */
static bool page_chunk_add(void)
{
	if (!page_pool_ready) {
		if (!page_max)
			page_max = page_ram_pages();
		page_chunk_slots = (page_max + PAGE_CHUNK_PAGES - 1) >> PAGE_CHUNK_SHIFT;
		page_chunks = page_chunk_slots ? calloc(page_chunk_slots, sizeof(*page_chunks)) : NULL;
		if (!page_chunks)
			return false;
		page_pool_ready = 1;
	}
	const unsigned long c = page_chunk_count;
	if (c >= page_chunk_slots)
		return false;
	const unsigned long first = c << PAGE_CHUNK_SHIFT;
	const unsigned long pages = page_max - first < PAGE_CHUNK_PAGES ? page_max - first :
		PAGE_CHUNK_PAGES;
	struct page_chunk *chunk = calloc(1, sizeof(*chunk));
	if (!chunk)
		return false;
	chunk->first = first;
	chunk->pages = pages;
	chunk->page = calloc(pages, sizeof(*chunk->page));
	chunk->order = malloc(pages * sizeof(*chunk->order));
	chunk->allocation = calloc(pages, sizeof(*chunk->allocation));
#ifdef LINUXU_DEXT_DK
	chunk->backing = calloc(pages, sizeof(*chunk->backing));
	chunk->dma = calloc(pages, sizeof(*chunk->dma));
	const bool ok = chunk->page && chunk->order && chunk->allocation && chunk->backing &&
		chunk->dma && va_resize(first + pages);
#else
	chunk->arena = aligned_alloc(LINUXU_PAGE_SIZE, pages * LINUXU_PAGE_SIZE);
	const bool ok = chunk->page && chunk->order && chunk->allocation && chunk->arena;
#endif
	if (!ok) {
		fprintf(stderr, "linuxu: page pool: no memory for descriptors of %lu more pages (%lu held)\n",
			pages, first);
		page_chunk_free(chunk);
		return false;
	}
	memset(chunk->order, -1, pages * sizeof(*chunk->order));
	for (unsigned long i = 0; i < pages; i++)
		chunk->page[i].index = first + i;
	__atomic_store_n(&page_chunks[c], chunk, __ATOMIC_RELEASE);
	__atomic_store_n(&page_chunk_count, c + 1, __ATOMIC_RELEASE);
	return true;
}

/* Pages the published chunks hold. */
static unsigned long page_pool_cap(void)
{
	const unsigned long count = __atomic_load_n(&page_chunk_count, __ATOMIC_ACQUIRE);
	if (!count)
		return 0;
	struct page_chunk *last = __atomic_load_n(&page_chunks[count - 1], __ATOMIC_ACQUIRE);
	return last->first + last->pages;
}

/* ---- page <-> pfn identity map (pfn == pool index) ---- */
struct page *pfn_to_page(unsigned long pfn)
{
	struct page_chunk *chunk = page_chunk_of(pfn);

	return chunk ? &chunk->page[pfn - chunk->first] : NULL;
}

static bool page_pool_index(const struct page *page, unsigned long *index)
{
	const uintptr_t address = (uintptr_t)page;
	const unsigned long count = __atomic_load_n(&page_chunk_count, __ATOMIC_ACQUIRE);

	if (!page)
		return false;
	for (unsigned long c = 0; c < count; c++) {
		const struct page_chunk *chunk = __atomic_load_n(&page_chunks[c], __ATOMIC_ACQUIRE);
		const uintptr_t base = (uintptr_t)chunk->page;

		if (address < base || address - base >= chunk->pages * sizeof(*page))
			continue;
		if ((address - base) % sizeof(*page))
			return false;
		*index = chunk->first + (address - base) / sizeof(*page);
		return true;
	}
	return false;
}

unsigned long page_to_pfn(const struct page *page)
{
	unsigned long index;
	return page_pool_index(page, &index) ? index : ~0UL;
}

unsigned long __page_to_pfn(const struct page *page)
{
	return page_to_pfn(page);
}

unsigned long __pfn_to_phys(unsigned long pfn)
{
	/* "physical" address = arena-relative; identity for the shim */
	return pfn << LINUXU_PAGE_SHIFT;
}

phys_addr_t pfn_to_phys(unsigned long pfn)
{
	return (phys_addr_t)__pfn_to_phys(pfn);
}

unsigned long virt_to_phys(unsigned long vaddr)
{
	struct page *p = virt_to_page_internal((void *)vaddr);
	return p ? (page_to_pfn(p) << LINUXU_PAGE_SHIFT) +
		(vaddr & (LINUXU_PAGE_SIZE - 1)) : 0;
}

unsigned long phys_to_virt(phys_addr_t addr)
{
	struct page *p = pfn_to_page((unsigned long)(addr >> LINUXU_PAGE_SHIFT));
	void *base = p ? page_address(p) : NULL;
	return base ? (unsigned long)base +
		((unsigned long)addr & (LINUXU_PAGE_SIZE - 1)) : 0;
}

bool pfn_valid(unsigned long pfn)
{
	return pfn_to_page(pfn) != NULL;
}

int page_to_nid(const struct page *page)
{
	(void)page;
	return 0;
}


void *page_address(const struct page *page)
{
	unsigned long pfn;
	if (!page_pool_index(page, &pfn)) return NULL;
#ifdef LINUXU_DEXT_DK
	return __atomic_load_n(&PG_BACKING(pfn), __ATOMIC_ACQUIRE);
#else
	const struct page_chunk *chunk = page_chunk_of(pfn);
	return __atomic_load_n(&PG_ALLOC(pfn), __ATOMIC_ACQUIRE) &&
		PG_ORDER(pfn) != -3 ? chunk->arena + (pfn - chunk->first) * LINUXU_PAGE_SIZE : NULL;
#endif
}

struct page *virt_to_page_internal(const void *vaddr)
{
#ifdef LINUXU_DEXT_DK
	struct page *p;
	if (!vaddr || !page_pool_ready)
		return NULL;
	pthread_mutex_lock(&page_lock);
	p = va_lookup((uintptr_t)vaddr);
	pthread_mutex_unlock(&page_lock);
	return p;
#else
	const uintptr_t address = (uintptr_t)vaddr;
	const unsigned long count = __atomic_load_n(&page_chunk_count, __ATOMIC_ACQUIRE);
	for (unsigned long c = 0; vaddr && c < count; c++) {
		const struct page_chunk *chunk = __atomic_load_n(&page_chunks[c], __ATOMIC_ACQUIRE);
		const uintptr_t base = (uintptr_t)chunk->arena;

		if (address < base || address - base >= chunk->pages * LINUXU_PAGE_SIZE)
			continue;
		const unsigned long pfn = chunk->first + (address - base) / LINUXU_PAGE_SIZE;
		return __atomic_load_n(&PG_ALLOC(pfn), __ATOMIC_ACQUIRE) &&
			PG_ORDER(pfn) != -3 ? PG(pfn) : NULL;
	}
	return NULL;
#endif
}


/* ---- page alloc/free ---- */
/*
 * alloc_pages: return the first `1<<order` pages in the pool with
 * refcount 0.  (Order>0 blocks are contiguous in pool index; the
 * arena backing them is contiguous malloc'd memory, so compound
 * arithmetic stays self-consistent.)
 */
/* A free run of @need pages inside one chunk, adding a chunk when none is
 * free; ~0UL when the pool cannot hold another. Caller holds page_lock. */
static unsigned long page_free_run_locked(unsigned long need)
{
	for (unsigned long c = 0;; c++) {
		if (c == page_chunk_count && !page_chunk_add())
			return ~0UL;
		const struct page_chunk *chunk = page_chunks[c];
		for (unsigned long at = 0; need <= chunk->pages && at <= chunk->pages - need; at += need) {
			unsigned long n;
			for (n = 0; n < need && !chunk->allocation[at + n]; n++) { }
			if (n == need)
				return chunk->first + at;
		}
	}
}

static struct page *alloc_pages_backing(gfp_t gfp, unsigned int order, bool cpu_only)
{
	unsigned long need, first;
	if (order > PAGE_CHUNK_SHIFT) return NULL;	/* more than a chunk: never contiguous */
	need = 1UL << order;
	pthread_mutex_lock(&page_lock);
	first = page_free_run_locked(need);
	if (first == ~0UL) goto failed;
	{
		unsigned long n;
		struct linuxu_page_allocation *allocation = calloc(1, sizeof(*allocation));
		if (!allocation) goto failed;
		allocation->first = first;
		allocation->count = allocation->live = need;
		allocation->order = order;
		allocation->compound = order && (gfp & __GFP_COMP);
		allocation->cpu_only = cpu_only;
#ifdef LINUXU_DEXT_DK
		allocation->cpu = cpu_only ? dext_cpu_alloc_pages(need * PAGE_SIZE) :
			linuxu_dma_alloc_coherent(NULL, need * PAGE_SIZE, &allocation->dma, gfp);
		if (!allocation->cpu) { free(allocation); goto failed; }
		PG_DMA(first) = allocation->dma;
#else
		const struct page_chunk *chunk = page_chunk_of(first);
		allocation->cpu = chunk->arena + (first - chunk->first) * PAGE_SIZE;
#endif
		for (n = 0; n < need; n++) {
			struct page *page = PG(first + n);
			PG_ALLOC(first + n) = allocation;
			PG_ORDER(first + n) = n ? -2 : (signed char)order;
			page->flags = 0;
			page->mapping = NULL;
			page->private = 0;
			page->index = first + n;
			page->compound_head = allocation->compound && n ? PG(first) : NULL;
			page->_compound = allocation->compound && !n;
			page->_pad1 = order;
			atomic_set(&page->refcount, allocation->compound && n ? 0 : 1);
#ifdef LINUXU_DEXT_DK
			PG_BACKING(first + n) = (char *)allocation->cpu + n * PAGE_SIZE;
			va_insert((uintptr_t)PG_BACKING(first + n), first + n);
#endif
		}
		if (gfp & __GFP_ZERO) memset(allocation->cpu, 0, need * PAGE_SIZE);
		pthread_mutex_unlock(&page_lock);
		return PG(first);
	}
failed:
	pthread_mutex_unlock(&page_lock);
	return NULL;
}

struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
	return alloc_pages_backing(gfp, order, false);
}
struct page *linuxu_alloc_cpu_page(gfp_t gfp)
{
	return alloc_pages_backing(gfp, 0, true);
}

static void page_retire_locked(unsigned long index, struct linuxu_page_allocation *allocation)
{
	if (PG_ALLOC(index) != allocation || PG_ORDER(index) == -3) return;
#ifdef LINUXU_DEXT_DK
	va_remove((uintptr_t)PG_BACKING(index));
	__atomic_store_n(&PG_BACKING(index), NULL, __ATOMIC_RELEASE);
	PG_DMA(index) = 0;
#endif
	/* Keep descriptor slots reserved until their shared backing is released. */
	PG_ORDER(index) = -3;
	atomic_set(&PG(index)->refcount, 0);
	PG(index)->mapping = NULL;
	PG(index)->private = 0;
	PG(index)->flags = 0;
	PG(index)->compound_head = NULL;
	PG(index)->_pad1 = 0;
	allocation->live--;
}

static void page_release_backing(struct linuxu_page_allocation *allocation)
{
	if (!allocation) return;
#ifdef LINUXU_DEXT_DK
	if (allocation->cpu_only) dext_cpu_free_pages(allocation->cpu, allocation->count * PAGE_SIZE);
	else linuxu_dma_free_coherent(allocation->device, allocation->count * PAGE_SIZE,
		allocation->cpu, allocation->dma);
#else
	if (allocation->attrs)
		linuxu_dma_unmap_page(allocation->device, allocation->dma,
			allocation->count * PAGE_SIZE, DMA_BIDIRECTIONAL);
#endif
	pthread_mutex_lock(&page_lock);
	for (unsigned long n = 0; n < allocation->count; n++) {
		unsigned long index = allocation->first + n;
		if (PG_ALLOC(index) == allocation) {
			__atomic_store_n(&PG_ALLOC(index), NULL, __ATOMIC_RELEASE);
			PG_ORDER(index) = -1;
		}
	}
	pthread_mutex_unlock(&page_lock);
	free(allocation);
}

/* Caller holds page_lock. Drop one owning reference and return an allocation
 * only after all its pages are released. Splitting changes logical ownership,
 * while one underlying IODMACommand remains until the final slice is unused. */
static struct linuxu_page_allocation *page_put_locked(struct page *page)
{
	unsigned long index;
	if (!page_pool_index(page, &index)) return NULL;
	struct linuxu_page_allocation *allocation = PG_ALLOC(index);
	if (!allocation) return NULL;
	if (allocation->compound) {
		index = allocation->first;
		page = PG(index);
	}
	int count = atomic_read(&page->refcount);
	if (count <= 0 || count == INT_MAX) return NULL;
	atomic_set(&page->refcount, count - 1);
	if (count != 1) return NULL;
	if (allocation->compound || (!allocation->split && index == allocation->first)) {
		for (unsigned long n = 0; n < allocation->count; n++) {
			unsigned long current = allocation->first + n;
			if (PG_ALLOC(current) != allocation) continue;
			if (!allocation->compound && current != index) {
				int refs = atomic_read(&PG(current)->refcount);
				if (refs > 1) { atomic_set(&PG(current)->refcount, refs - 1); continue; }
			}
			page_retire_locked(current, allocation);
		}
	} else {
		page_retire_locked(index, allocation);
	}
	return allocation->live ? NULL : allocation;
}

bool linuxu_get_page(struct page *page)
{
	unsigned long index;
	bool result = false;
	pthread_mutex_lock(&page_lock);
	if (page_pool_index(page, &index) && PG_ALLOC(index) && PG_ORDER(index) != -3) {
		struct linuxu_page_allocation *allocation = PG_ALLOC(index);
		if (allocation->compound) page = PG(allocation->first);
		int count = atomic_read(&page->refcount);
		if (count > 0) {
			if (count != INT_MAX) atomic_set(&page->refcount, count + 1);
			result = true;
		}
	}
	pthread_mutex_unlock(&page_lock);
	return result;
}

void linuxu_put_page(struct page *page)
{
	pthread_mutex_lock(&page_lock);
	struct linuxu_page_allocation *allocation = page_put_locked(page);
	pthread_mutex_unlock(&page_lock);
	page_release_backing(allocation);
}

void __free_pages(struct page *page, unsigned int order)
{
	unsigned long index;
	struct linuxu_page_allocation *released = NULL;
	pthread_mutex_lock(&page_lock);
	if (order < sizeof(unsigned long) * 8 && page_pool_index(page, &index)) {
		struct linuxu_page_allocation *allocation = PG_ALLOC(index);
		if (allocation && ((allocation->split && !order) ||
			(index == allocation->first && order == allocation->order)))
			released = page_put_locked(page);
	}
	pthread_mutex_unlock(&page_lock);
	page_release_backing(released);
}

void split_page(struct page *page, unsigned int order)
{
	unsigned long index;
	pthread_mutex_lock(&page_lock);
	if (page_pool_index(page, &index)) {
		struct linuxu_page_allocation *allocation = PG_ALLOC(index);
		if (allocation && !allocation->compound && !allocation->split &&
			index == allocation->first && order == allocation->order) {
			allocation->split = true;
			for (unsigned long n = 0; n < allocation->count; n++) {
				PG_ORDER(index + n) = 0;
				PG(index + n)->_pad1 = 0;
			}
		}
	}
	pthread_mutex_unlock(&page_lock);
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle,
		gfp_t gfp, unsigned long attrs)
{
	(void)attrs;
	if (!size || !handle || size > SIZE_MAX - (PAGE_SIZE - 1)) return NULL;
	*handle = DMA_MAPPING_ERROR;
	unsigned int order = get_order(size);
	struct page *page = alloc_pages(gfp | __GFP_ZERO, order);
	if (!page) return NULL;
	unsigned long index = page_to_pfn(page);
	struct linuxu_page_allocation *allocation = PG_ALLOC(index);
#ifdef LINUXU_DEXT_DK
	dma_addr_t address = allocation->dma;
	u64 mask = dev ? dev->coherent_dma_mask : ~0ULL;
	if (dev && dev->bus_dma_limit && dev->bus_dma_limit < mask) mask = dev->bus_dma_limit;
	if ((gfp & __GFP_DMA32) && mask > UINT32_MAX) mask = UINT32_MAX;
	if (address > mask || allocation->count * PAGE_SIZE - 1 > mask - address) {
		__free_pages(page, order);
		return NULL;
	}
#else
	dma_addr_t address = linuxu_dma_map_page(dev, page, 0, allocation->count * PAGE_SIZE,
		DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, address)) { __free_pages(page, order); return NULL; }
#endif
	allocation->attrs = true;
	allocation->device = dev;
	allocation->requested = size;
	allocation->dma = address;
	*handle = address;
	return allocation->cpu;
}

void dma_free_attrs(struct device *dev, size_t size, void *cpu,
		dma_addr_t handle, unsigned long attrs)
{
	(void)dev; (void)attrs;
	struct page *page = virt_to_page_internal(cpu);
	struct linuxu_page_allocation *released = NULL;
	unsigned long index;
	pthread_mutex_lock(&page_lock);
	if (page_pool_index(page, &index)) {
		struct linuxu_page_allocation *allocation = PG_ALLOC(index);
		if (allocation && allocation->attrs && allocation->cpu == cpu &&
			allocation->requested == size && allocation->dma == handle)
			released = page_put_locked(page);
	}
	pthread_mutex_unlock(&page_lock);
	page_release_backing(released);
}

void linuxu_lock_page(struct page *page)
{
	pthread_mutex_lock(&page_lock);
	while (__atomic_fetch_or(&page->flags, 1UL << 9, __ATOMIC_ACQUIRE) & (1UL << 9))
		pthread_cond_wait(&page_changed, &page_lock);
	pthread_mutex_unlock(&page_lock);
}
void linuxu_unlock_page(struct page *page)
{
	pthread_mutex_lock(&page_lock);
	__atomic_fetch_and(&page->flags, ~(1UL << 9), __ATOMIC_RELEASE);
	pthread_cond_broadcast(&page_changed);
	pthread_mutex_unlock(&page_lock);
}
bool linuxu_trylock_page(struct page *page)
{
	return !(__atomic_fetch_or(&page->flags, 1UL << 9, __ATOMIC_ACQUIRE) & (1UL << 9));
}

void __free_pages_ok(struct page *page, unsigned int order)
{
	__free_pages(page, order);
}

void free_pages(unsigned long address, unsigned int order)
{
	/* free by address: find the pool page whose backing slot matches */
	struct page *p;

	if (order >= sizeof(unsigned long) * 8 || (address & (LINUXU_PAGE_SIZE - 1)))
		return;
	p = virt_to_page((void *)address);
	if (p && page_to_pfn(p) + (1UL << order) <= page_pool_cap())
		__free_pages(p, order);
}

unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	struct page *p = alloc_pages(gfp_mask, order);

	return p ? (unsigned long)page_address(p) : 0;
}

/* vm_area_alloc/vm_area_free/vma_munmap live in mm.c; vma_pages is inline. */

/* single-node host: all PFNs are "ram" on node 0 (vendor 2026 mmzone.h).
 * page_to_virt resolves arena pages through page_address(); anything
 * outside the arena is treated as an equal-address kernel pointer. */
int page_is_ram(unsigned long pfn)
{
	return pfn_to_page(pfn) != NULL;
}

int num_possible_nodes(void)
{
	return 1;
}

void *page_to_virt(struct page *page)
{
	return page_address(page);
}

/* ---- node / per-cpu variants (W12: single host node; identity) ---- */
struct page *alloc_node_gfp(int nid, gfp_t gfp)
{
	(void)nid;
	return alloc_pages(gfp, 0);
}

/* alloc_page: the header (<linux/mm.h>) provides the canonical inline
 * alloc_page(gfp) { return alloc_pages(gfp, 0); } and this .c must not
 * redefine it (that broke the page.o build); the inline is the only
 * definition. */
