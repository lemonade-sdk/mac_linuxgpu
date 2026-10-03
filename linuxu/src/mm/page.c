/* linuxu shim: stable struct page IDs and 16 KiB backing. Native tests use
 * a reserved host arena; DriverKit allocates backing only for live blocks. */
#include <pthread.h>
#include <stdint.h>
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
#endif

/* 16 KB page contract */
#define LINUXU_PAGE_SHIFT 14
#define LINUXU_PAGE_SIZE  (1UL << LINUXU_PAGE_SHIFT)

#define LINUXU_MAX_PAGES 131072  /* default 2 GB arena of page structs (covers the 1.5 GB DART budget ceiling test) */

static struct page *page_pool;
static signed char *page_block_order;
struct linuxu_page_allocation {
	unsigned long first, count, live;
	unsigned int order;
	bool compound, split, cpu_only, attrs;
	void *cpu;
	dma_addr_t dma;
	size_t requested;
	struct device *device;
};
static struct linuxu_page_allocation **page_allocations;
static pthread_cond_t page_changed = PTHREAD_COND_INITIALIZER;
#ifdef LINUXU_DEXT_DK
/* DriverKit cannot commit a 2 GiB arena just to mint synthetic page IDs. */
static void **page_backing;
static dma_addr_t *page_block_dma;
#define LINUXU_VA_SLOTS (LINUXU_MAX_PAGES * 2)
struct linuxu_va_slot {
	uintptr_t base;
	unsigned long pfn;
};
static struct linuxu_va_slot *va_slots;
#else
static unsigned char *arena;
#endif
static unsigned long arena_pages;
static unsigned long pool_capacity;   /* allocated pool/arena size */
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
static int page_pool_ready;

/* Test hook: choose descriptor capacity before the first allocation. */
int linuxu_page_pool_extend(unsigned long pages)
{
	if (pages > SIZE_MAX / LINUXU_PAGE_SIZE ||
	    pages > SIZE_MAX / sizeof(struct page))
		return -1;
	pthread_mutex_lock(&page_lock);
	if (pages <= arena_pages)
		goto done;
#ifdef LINUXU_DEXT_DK
	if (pages > LINUXU_MAX_PAGES) {
		pthread_mutex_unlock(&page_lock);
		return -1;
	}
#endif
	if (page_pool_ready && pages > pool_capacity) {
		pthread_mutex_unlock(&page_lock);
		return -1; /* can't grow after first use */
	}
	arena_pages = pages;
done:
	pthread_mutex_unlock(&page_lock);
	return 0;
}
/*
 * Descriptor capacity is fixed at first use so struct page * remains stable.
 * Native tests reserve a virtual arena; DriverKit stores only per-page VA
 * metadata here and allocates backing separately in alloc_pages().
 */
static void page_pool_init(void)
{
	unsigned long max = arena_pages ? arena_pages : LINUXU_MAX_PAGES;

	if (!page_pool_ready) {
		page_pool = calloc(max, sizeof(struct page));
		page_block_order = malloc(max * sizeof(*page_block_order));
		page_allocations = calloc(max, sizeof(*page_allocations));
#ifdef LINUXU_DEXT_DK
		page_backing = calloc(max, sizeof(*page_backing));
		page_block_dma = calloc(max, sizeof(*page_block_dma));
		va_slots = calloc(LINUXU_VA_SLOTS, sizeof(*va_slots));
		if (page_pool && page_backing && page_block_order &&
		    page_block_dma && va_slots && page_allocations) {
#else
		arena = aligned_alloc(LINUXU_PAGE_SIZE, max * LINUXU_PAGE_SIZE);
		if (page_pool && arena && page_block_order && page_allocations) {
#endif
			unsigned long i;
			memset(page_block_order, -1, max * sizeof(*page_block_order));

			for (i = 0; i < max; i++) {
				struct page *p = &page_pool[i];

				p->flags = 0;
				p->mapping = NULL;
				p->private = 0;
				p->index = i;
				atomic_set(&p->refcount, 0);
			}
			pool_capacity = max;
			page_pool_ready = 1;
		} else {
			free(page_pool);
			page_pool = NULL;
			free(page_block_order);
			page_block_order = NULL;
			free(page_allocations);
			page_allocations = NULL;
#ifdef LINUXU_DEXT_DK
			free(page_backing);
			page_backing = NULL;
			free(page_block_dma);
			page_block_dma = NULL;
			free(va_slots);
			va_slots = NULL;
#else
			free(arena);
			arena = NULL;
#endif
		}
	}
}

#ifdef LINUXU_DEXT_DK
/* Open-addressed lookup for the 16 KiB-aligned pages in live allocations.
 * Empty=0 and tombstone=1; all real aligned VAs are greater than one. */
static unsigned long va_hash(uintptr_t base)
{
	return ((base >> LINUXU_PAGE_SHIFT) * 11400714819323198485ull) &
		(LINUXU_VA_SLOTS - 1);
}

static void va_insert(uintptr_t base, unsigned long pfn)
{
	unsigned long pos = va_hash(base);
	while (va_slots[pos].base > 1)
		pos = (pos + 1) & (LINUXU_VA_SLOTS - 1);
	va_slots[pos] = (struct linuxu_va_slot){ base, pfn };
}

static void va_remove(uintptr_t base)
{
	unsigned long pos = va_hash(base);
	for (unsigned long scanned = 0; scanned < LINUXU_VA_SLOTS &&
	     va_slots[pos].base; scanned++) {
		if (va_slots[pos].base == base) {
			va_slots[pos].base = 1;
			return;
		}
		pos = (pos + 1) & (LINUXU_VA_SLOTS - 1);
	}
}

static struct page *va_lookup(uintptr_t addr)
{
	uintptr_t base = addr & ~(uintptr_t)(LINUXU_PAGE_SIZE - 1);
	unsigned long pos = va_hash(base);
	/* Freed addresses leave tombstones. After enough distinct allocations
	 * there may be no empty slot, even though no live page matches. */
	for (unsigned long scanned = 0; scanned < LINUXU_VA_SLOTS &&
	     va_slots[pos].base; scanned++) {
		if (va_slots[pos].base == base)
			return &page_pool[va_slots[pos].pfn];
		pos = (pos + 1) & (LINUXU_VA_SLOTS - 1);
	}
	return NULL;
}
#endif


/* the live cap of the pool: min(extend cap, allocated capacity);
 * arena_pages is 0 unless a test explicitly extended the pool. */
static unsigned long page_pool_cap(void)
{
	unsigned long cap = pool_capacity;

	if (arena_pages && arena_pages < cap)
		cap = arena_pages;
	return cap;
}

/* ---- page <-> pfn identity map (pfn == pool index) ---- */
struct page *pfn_to_page(unsigned long pfn)
{
	if (!page_pool_ready || pfn >= page_pool_cap())
		return NULL;
	return &page_pool[pfn];
}

static bool page_pool_index(const struct page *page, unsigned long *index)
{
	uintptr_t address = (uintptr_t)page, base = (uintptr_t)page_pool;
	if (!page || !page_pool || address < base ||
	    (address - base) % sizeof(*page) ||
	    (address - base) / sizeof(*page) >= pool_capacity)
		return false;
	*index = (address - base) / sizeof(*page);
	return true;
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
	return __atomic_load_n(&page_backing[pfn], __ATOMIC_ACQUIRE);
#else
	return __atomic_load_n(&page_allocations[pfn], __ATOMIC_ACQUIRE) &&
		page_block_order[pfn] != -3 ? arena + pfn * LINUXU_PAGE_SIZE : NULL;
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
	uintptr_t address = (uintptr_t)vaddr, base = (uintptr_t)arena;
	if (!arena || address < base ||
	    address - base >= page_pool_cap() * LINUXU_PAGE_SIZE)
		return NULL;
	struct page *page = &page_pool[(address - base) / LINUXU_PAGE_SIZE];
	return __atomic_load_n(&page_allocations[page - page_pool], __ATOMIC_ACQUIRE) &&
		page_block_order[page - page_pool] != -3 ? page : NULL;
#endif
}


/* ---- page alloc/free ---- */
/*
 * alloc_pages: return the first `1<<order` pages in the pool with
 * refcount 0.  (Order>0 blocks are contiguous in pool index; the
 * arena backing them is contiguous malloc'd memory, so compound
 * arithmetic stays self-consistent.)
 */
static struct page *alloc_pages_backing(gfp_t gfp, unsigned int order, bool cpu_only)
{
	unsigned long need, first;
	if (order >= sizeof(unsigned long) * 8 - LINUXU_PAGE_SHIFT) return NULL;
	need = 1UL << order;
	pthread_mutex_lock(&page_lock);
	page_pool_init();
	if (!page_pool_ready || need > page_pool_cap()) goto failed;
	for (first = 0; first <= page_pool_cap() - need; first += need) {
		unsigned long n;
		for (n = 0; n < need && !page_allocations[first + n]; n++) { }
		if (n != need) continue;
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
		page_block_dma[first] = allocation->dma;
#else
		allocation->cpu = arena + first * PAGE_SIZE;
#endif
		for (n = 0; n < need; n++) {
			struct page *page = &page_pool[first + n];
			page_allocations[first + n] = allocation;
			page_block_order[first + n] = n ? -2 : (signed char)order;
			page->flags = 0;
			page->mapping = NULL;
			page->private = 0;
			page->index = first + n;
			page->compound_head = allocation->compound && n ? &page_pool[first] : NULL;
			page->_compound = allocation->compound && !n;
			page->_pad1 = order;
			atomic_set(&page->refcount, allocation->compound && n ? 0 : 1);
#ifdef LINUXU_DEXT_DK
			page_backing[first + n] = (char *)allocation->cpu + n * PAGE_SIZE;
			va_insert((uintptr_t)page_backing[first + n], first + n);
#endif
		}
		if (gfp & __GFP_ZERO) memset(allocation->cpu, 0, need * PAGE_SIZE);
		pthread_mutex_unlock(&page_lock);
		return &page_pool[first];
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
	if (page_allocations[index] != allocation || page_block_order[index] == -3) return;
#ifdef LINUXU_DEXT_DK
	va_remove((uintptr_t)page_backing[index]);
	__atomic_store_n(&page_backing[index], NULL, __ATOMIC_RELEASE);
	page_block_dma[index] = 0;
#endif
	/* Keep descriptor slots reserved until their shared backing is released. */
	page_block_order[index] = -3;
	atomic_set(&page_pool[index].refcount, 0);
	page_pool[index].mapping = NULL;
	page_pool[index].private = 0;
	page_pool[index].flags = 0;
	page_pool[index].compound_head = NULL;
	page_pool[index]._pad1 = 0;
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
		if (page_allocations[index] == allocation) {
			__atomic_store_n(&page_allocations[index], NULL, __ATOMIC_RELEASE);
			page_block_order[index] = -1;
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
	struct linuxu_page_allocation *allocation = page_allocations[index];
	if (!allocation) return NULL;
	if (allocation->compound) {
		index = allocation->first;
		page = &page_pool[index];
	}
	int count = atomic_read(&page->refcount);
	if (count <= 0 || count == INT_MAX) return NULL;
	atomic_set(&page->refcount, count - 1);
	if (count != 1) return NULL;
	if (allocation->compound || (!allocation->split && index == allocation->first)) {
		for (unsigned long n = 0; n < allocation->count; n++) {
			unsigned long current = allocation->first + n;
			if (page_allocations[current] != allocation) continue;
			if (!allocation->compound && current != index) {
				int refs = atomic_read(&page_pool[current].refcount);
				if (refs > 1) { atomic_set(&page_pool[current].refcount, refs - 1); continue; }
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
	if (page_pool_index(page, &index) && page_allocations[index] && page_block_order[index] != -3) {
		struct linuxu_page_allocation *allocation = page_allocations[index];
		if (allocation->compound) page = &page_pool[allocation->first];
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
		struct linuxu_page_allocation *allocation = page_allocations[index];
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
		struct linuxu_page_allocation *allocation = page_allocations[index];
		if (allocation && !allocation->compound && !allocation->split &&
			index == allocation->first && order == allocation->order) {
			allocation->split = true;
			for (unsigned long n = 0; n < allocation->count; n++) {
				page_block_order[index + n] = 0;
				page_pool[index + n]._pad1 = 0;
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
	struct linuxu_page_allocation *allocation = page_allocations[index];
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
		struct linuxu_page_allocation *allocation = page_allocations[index];
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
