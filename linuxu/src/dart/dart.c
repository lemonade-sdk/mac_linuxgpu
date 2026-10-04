/* linuxu shim: dart — DMA→DART mapping layer. The host test backend uses
 * identity IOVAs. DriverKit coherent allocations use IODMACommand directly;
 * streaming mappings use IODMACommand-backed bounce buffers because the SDK
 * cannot wrap arbitrary shim memory in an IOMemoryDescriptor.
 *
 * There is no software ceiling on mapped bytes, as in Linux: what limits
 * DMA is the platform, whose refusals (memory, wiring, the DART's IOVA
 * window) the DriverKit seam reports as a failed mapping, ENOMEM here. The
 * used/peak counters account live-mapped bytes for diagnostics.
 *
 * Live mappings track their allocation and alias ownership. DriverKit
 * retains their accounting when completion fails; the host backend uses
 * a token hash for simulated mappings. Accounting is done inside the lock
 * that covers table insertion, so the used/peak counters are race-free
 * across the shim's worker threads. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>      /* struct page, page_address, PAGE_SIZE */
#include <linux/errno.h>
#include <rt/dart.h>

#ifdef LINUXU_DEXT_DK
/* driverKit build: the real DART IOVA comes from the DriverKit IODMACommand
 * seam (dext/sources/iokit_bridge.mm), not an identity host VA.  The
 * accounting below is the same in both builds. */
#include <rt/dext_dma.h>
#endif

/* 16 KB coherent alignment (P2 contract; firmware/ucode load needs it). */
#define DART_COHERENT_ALIGN 0x4000UL

/* No fixed addressing limit lives here: a device's reach is its own DMA
 * masks (dma_mask.c records them, Linux-style), and the DriverKit seam
 * creates every IODMACommand with the bound device's width and refuses
 * mappings beyond it.  Each mapping made for a device is additionally
 * checked against that device's masks and bus limit below. */
#define DART_NO_LIMIT UINT64_MAX

#ifdef LINUXU_DEXT_DK
/* The GPU page table addresses host mappings in 4 KB units even though the
 * Apple host pages are 16 KB. */
#define DART_GPU_PAGE_SIZE 0x1000ULL
static int dart_iova_valid(uint64_t iova, uint64_t size)
{
	return iova && !(iova & (DART_GPU_PAGE_SIZE - 1)) && size &&
		size - 1 <= DART_NO_LIMIT - iova;
}
#endif

/* ---- accounting ---- */
static pthread_mutex_t dart_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dart_used;
static uint64_t dart_peak;

/* caller holds dart_lock */
static void dart_charge(uint64_t bytes)
{
	dart_used += bytes;
	if (dart_used > dart_peak)
		dart_peak = dart_used;
}

static void dart_refund(uint64_t bytes)
{
	if (dart_used >= bytes)
		dart_used -= bytes;
}

/* ---- holding mappings an engine may still use ----
 * A DMA mapping the driver releases while a GPU engine still has work
 * queued against it becomes a stray device access once that work runs: a
 * DART fault (which can take a Thunderbolt device off the bus) or, if the
 * IOVA was handed out again, a write into someone else's memory. Upstream
 * releases such memory itself when a timed-out move gives up (TTM's
 * ttm_bo_wait_free_node after 15 s), counting on a GPU reset to cancel the
 * work, and with recovery off nothing cancels it. While the registered
 * predicate reports a stalled engine, releases are held: the mapping (and
 * its backing) stays live and is released once no engine is stalled, or
 * when the predicate is removed. */
static pthread_mutex_t dart_hold_lock = PTHREAD_MUTEX_INITIALIZER;
static bool (*dart_hold_fn)(void *);
static void *dart_hold_arg;
static unsigned int dart_held_count;	/* dart_lock */

static bool dart_hold_active(void)
{
	bool active = false;

	pthread_mutex_lock(&dart_hold_lock);
	if (dart_hold_fn)
		active = dart_hold_fn(dart_hold_arg);
	pthread_mutex_unlock(&dart_hold_lock);
	return active;
}

static void dart_release_held_now(void);

/* Release what was held once no engine is stalled any more. */
static void dart_release_held_if_idle(void)
{
	unsigned int held;

	pthread_mutex_lock(&dart_lock);
	held = dart_held_count;
	pthread_mutex_unlock(&dart_lock);
	if (held && !dart_hold_active())
		dart_release_held_now();
}

/* ---- token table (hash of live mappings) ---- */
struct dart_token {
	struct dart_token *next;
	uint64_t iova, size, length;
	enum dma_data_direction direction;
	bool coherent;
};
#define DART_TABLE_BUCKETS 4096
static struct dart_token *dart_table[DART_TABLE_BUCKETS];
static int dart_table_count;

#ifdef LINUXU_DEXT_DK
/* A coherent allocation can be mapped again by TTM without making a copy.
 * Keep the original DART mapping alive until all streaming aliases go away. */
struct dart_coherent {
	struct dart_coherent *next;
	void *cpu;
	uint64_t iova;
	size_t size;
	unsigned int aliases;
	int freeing;
	int releasing;
	int held;	/* freed while an engine was stalled */
};
static struct dart_coherent *dart_coherents;
static unsigned int dart_allocations_inflight;

static struct dart_coherent *dart_coherent_by_cpu(const void *source,
						 size_t size)
{
	uintptr_t p = (uintptr_t)source;
	for (struct dart_coherent *e = dart_coherents; e; e = e->next) {
		uintptr_t base = (uintptr_t)e->cpu;
		if (p >= base && p - base < e->size)
			return e;
	}
	return NULL;
}

static struct dart_coherent *dart_coherent_by_iova(uint64_t iova,
						  size_t size)
{
	for (struct dart_coherent *e = dart_coherents; e; e = e->next)
		if (iova >= e->iova && iova - e->iova < e->size &&
		    size <= e->size - (iova - e->iova))
			return e;
	return NULL;
}

/* caller holds dart_lock */
static struct dart_coherent *dart_coherent_detach(struct dart_coherent *e)
{
	struct dart_coherent **it = &dart_coherents;
	while (*it && *it != e)
		it = &(*it)->next;
	if (*it) *it = e->next;
	dart_table_count--;
	dart_refund(e->size);
	return e;
}

/* The caller marks releasing while holding dart_lock. A failed completion
 * keeps both the allocation record and its accounted bytes for quarantine. */
static void dart_coherent_release(struct dart_coherent *e)
{
	int result = dext_dma_free_coherent(e->cpu, e->size);
	pthread_mutex_lock(&dart_lock);
	if (result) {
		e->releasing = 0;
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	dart_coherent_detach(e);
	pthread_mutex_unlock(&dart_lock);
	free(e);
}

/* A free from the driver: held while an engine is stalled. The caller set
 * releasing under dart_lock. */
static void dart_coherent_retire(struct dart_coherent *e)
{
	if (dart_hold_active()) {
		pthread_mutex_lock(&dart_lock);
		e->releasing = 0;
		if (!e->held) {
			e->held = 1;
			dart_held_count++;
		}
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	dart_coherent_release(e);
}

struct dart_alias {
	struct dart_alias *next;
	struct dart_coherent *owner;
	uint64_t iova;
	size_t size;
	enum dma_data_direction direction;
};
static struct dart_alias *dart_aliases;

/* DriverKit cannot build an IOMemoryDescriptor from arbitrary shim memory.
 * Streaming mappings therefore use a coherent IODMACommand-backed buffer and
 * copy at the Linux DMA ownership boundaries. */
struct dart_stream {
	uint64_t iova;
	void *source;
	void *bounce;
	size_t length;
	size_t charged;
	enum dma_data_direction direction;
	int releasing;
	int held;	/* unmapped while an engine was stalled */
};
/* Slots grow in chunks that never move (a slot's address stays valid while
 * dart_lock is dropped), as many as mappings are live at once. */
#define DART_STREAM_CHUNK 256
static struct dart_stream **dart_stream_chunks;	/* dart_lock */
static unsigned int dart_stream_nchunks;
static unsigned int dart_stream_capacity;

static struct dart_stream *dart_stream_at(unsigned int i)
{
	return &dart_stream_chunks[i / DART_STREAM_CHUNK][i % DART_STREAM_CHUNK];
}

/* A free slot, reserved (releasing) for the caller; -1 when no memory is
 * left for another chunk. Caller holds dart_lock. */
static int dart_stream_reserve_locked(void)
{
	for (unsigned int i = 0; i < dart_stream_capacity; ++i) {
		struct dart_stream *st = dart_stream_at(i);
		if (!st->iova && !st->releasing) {
			st->releasing = 1;
			return (int)i;
		}
	}
	if (dart_stream_capacity > (unsigned int)INT32_MAX - DART_STREAM_CHUNK)
		return -1;
	struct dart_stream **chunks = realloc(dart_stream_chunks,
		(dart_stream_nchunks + 1) * sizeof(*chunks));
	if (!chunks)
		return -1;
	dart_stream_chunks = chunks;
	struct dart_stream *chunk = calloc(DART_STREAM_CHUNK, sizeof(*chunk));
	if (!chunk)
		return -1;
	dart_stream_chunks[dart_stream_nchunks++] = chunk;
	const unsigned int slot = dart_stream_capacity;
	dart_stream_capacity += DART_STREAM_CHUNK;
	chunk[0].releasing = 1;
	return (int)slot;
}

static struct dart_stream *dart_stream_find(uint64_t iova)
{
	if (!iova) return NULL;
	for (unsigned int i = 0; i < dart_stream_capacity; ++i)
		if (dart_stream_at(i)->iova == iova)
			return dart_stream_at(i);
	return NULL;
}

static bool dart_stream_range_valid(struct device *dev, uint64_t iova, size_t size)
{
	uint64_t mask = DART_NO_LIMIT;
	if (dev) {
		mask = dev->dma_mask && *dev->dma_mask ? *dev->dma_mask : UINT32_MAX;
		if (dev->bus_dma_limit && dev->bus_dma_limit < mask)
			mask = dev->bus_dma_limit;
	}
	return size && iova <= mask && size - 1 <= mask - iova;
}

dma_addr_t linuxu_dma_map_single(struct device *dev, void *source,
				 size_t size, enum dma_data_direction dir)
{
	void *bounce = NULL;
	uint64_t iova = 0;
	size_t charged;
	int slot = -1;
	if (!source || !size || !valid_dma_direction(dir) ||
	    size > SIZE_MAX - (DART_COHERENT_ALIGN - 1))
		return (dma_addr_t)(uintptr_t)-EINVAL;
	pthread_mutex_lock(&dart_lock);
	struct dart_coherent *coherent = dart_coherent_by_cpu(source, size);
	if (coherent) {
		uint64_t alias = coherent->iova +
			((uintptr_t)source - (uintptr_t)coherent->cpu);
		if (coherent->freeing ||
		    size > coherent->size - ((uintptr_t)source -
					    (uintptr_t)coherent->cpu) ||
		    !alias || size - 1 > DART_NO_LIMIT - alias ||
		    !dart_stream_range_valid(dev, alias, size)) {
			pthread_mutex_unlock(&dart_lock);
			return (dma_addr_t)(uintptr_t)-EINVAL;
		}
		struct dart_alias *alias_record = malloc(sizeof(*alias_record));
		if (!alias_record || coherent->aliases == UINT32_MAX) {
			free(alias_record);
			pthread_mutex_unlock(&dart_lock);
			return (dma_addr_t)(uintptr_t)-ENOMEM;
		}
		*alias_record = (struct dart_alias){dart_aliases, coherent, alias, size, dir};
		dart_aliases = alias_record;
		coherent->aliases++;
		pthread_mutex_unlock(&dart_lock);
		return (dma_addr_t)alias;
	}
	pthread_mutex_unlock(&dart_lock);
	charged = (size + DART_COHERENT_ALIGN - 1) & ~(DART_COHERENT_ALIGN - 1);
	pthread_mutex_lock(&dart_lock);
	slot = dart_stream_reserve_locked(); /* reserved across DriverKit calls */
	if (slot >= 0) dart_charge(charged);
	pthread_mutex_unlock(&dart_lock);
	if (slot < 0) return (dma_addr_t)(uintptr_t)-ENOMEM;
	if (dext_dma_alloc_coherent(charged, &bounce, &iova) != 0 || !bounce ||
	    !dart_iova_valid(iova, charged) || !dart_stream_range_valid(dev, iova, charged)) {
		int release_failed = bounce && dext_dma_free_coherent(bounce, charged);
		pthread_mutex_lock(&dart_lock);
		if (release_failed) {
			*dart_stream_at(slot) = (struct dart_stream){.iova = iova,
				.bounce = bounce, .charged = charged, .releasing = 1};
			dart_table_count++;
		} else {
			memset(dart_stream_at(slot), 0, sizeof(struct dart_stream));
			dart_refund(charged);
		}
		pthread_mutex_unlock(&dart_lock);
		return (dma_addr_t)(uintptr_t)-ENOMEM;
	}
	if (dir != DMA_FROM_DEVICE) memcpy(bounce, source, size);
	pthread_mutex_lock(&dart_lock);
	*dart_stream_at(slot) = (struct dart_stream){iova, source, bounce, size, charged, dir};
	dart_table_count++;
	pthread_mutex_unlock(&dart_lock);
	return (dma_addr_t)iova;
}

void linuxu_dma_unmap_single(struct device *dev, dma_addr_t address,
			      size_t size, enum dma_data_direction dir)
{
	struct dart_stream entry;
	(void)dev;
	if (!address || !size || !valid_dma_direction(dir)) return;
	pthread_mutex_lock(&dart_lock);
	struct dart_alias **alias_link = &dart_aliases;
	while (*alias_link && ((*alias_link)->iova != address ||
	       (*alias_link)->size != size || (*alias_link)->direction != dir))
		alias_link = &(*alias_link)->next;
	struct dart_alias *alias = *alias_link;
	if (alias) {
		struct dart_coherent *coherent = alias->owner;
		*alias_link = alias->next;
		coherent->aliases--;
		if (coherent->freeing && !coherent->aliases) {
			coherent->releasing = 1;
			pthread_mutex_unlock(&dart_lock);
			dart_coherent_retire(coherent);
		} else {
			pthread_mutex_unlock(&dart_lock);
		}
		free(alias);
		return;
	}
	struct dart_stream *found = dart_stream_find((uint64_t)address);
	if (!found || found->releasing || found->length != size ||
	    found->direction != dir) { pthread_mutex_unlock(&dart_lock); return; }
	found->releasing = 1;
	entry = *found;
	pthread_mutex_unlock(&dart_lock);
	if (entry.direction != DMA_TO_DEVICE)
		memcpy(entry.source, entry.bounce, entry.length);
	if (dart_hold_active()) {
		/* The bounce buffer stays mapped; no sync reaches it again. */
		pthread_mutex_lock(&dart_lock);
		if (!found->held) {
			found->held = 1;
			dart_held_count++;
		}
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	int result = dext_dma_free_coherent(entry.bounce, entry.charged);
	pthread_mutex_lock(&dart_lock);
	if (!result) {
		memset(found, 0, sizeof(*found));
		dart_refund(entry.charged);
		dart_table_count--;
	}
	/* On failure keep releasing set: the source may be freed by the caller
	 * after unmap, so no later sync may copy to or from that source. */
	pthread_mutex_unlock(&dart_lock);
}

void linuxu_dma_sync_single_for_cpu(struct device *dev, dma_addr_t address,
				    size_t size, enum dma_data_direction dir)
{
	(void)dev; (void)dir;
	pthread_mutex_lock(&dart_lock);
	struct dart_stream *entry = dart_stream_find((uint64_t)address);
	if (entry && !entry->releasing && entry->direction != DMA_TO_DEVICE)
		memcpy(entry->source, entry->bounce,
		       size < entry->length ? size : entry->length);
	pthread_mutex_unlock(&dart_lock);
}

void linuxu_dma_sync_single_for_device(struct device *dev, dma_addr_t address,
				       size_t size, enum dma_data_direction dir)
{
	(void)dev; (void)dir;
	pthread_mutex_lock(&dart_lock);
	struct dart_stream *entry = dart_stream_find((uint64_t)address);
	if (entry && !entry->releasing && entry->direction != DMA_FROM_DEVICE)
		memcpy(entry->bounce, entry->source,
		       size < entry->length ? size : entry->length);
	pthread_mutex_unlock(&dart_lock);
}

void *linuxu_dma_cpu_address(dma_addr_t address)
{
	void *source = (void *)(uintptr_t)address;
	pthread_mutex_lock(&dart_lock);
	struct dart_coherent *coherent = dart_coherent_by_iova((uint64_t)address, 1);
	if (coherent)
		source = (uint8_t *)coherent->cpu +
			((uint64_t)address - coherent->iova);
	struct dart_stream *entry = dart_stream_find((uint64_t)address);
	if (entry) source = entry->releasing ? NULL : entry->source;
	pthread_mutex_unlock(&dart_lock);
	return source;
}

void *linuxu_sg_cpu_address(struct scatterlist *sg)
{
	if (!sg) return NULL;
	if (sg->page) {
		void *page = page_address(sg->page);
		return page ? (uint8_t *)page + sg->offset : NULL;
	}
	return linuxu_dma_cpu_address(sg->dma_address);
}

int linuxu_dma_map_sgtable(struct device *dev, struct sg_table *sgt,
			    enum dma_data_direction dir)
{
	if (!sgt || !sgt->sgl || !sgt->orig_nents) return -EINVAL;
	int mapped = linuxu_dma_map_sg(dev, sgt->sgl, sgt->orig_nents, dir);
	if (!mapped) return -ENOMEM;
	sgt->nents = mapped;
	return 0;
}

void linuxu_dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
			       enum dma_data_direction dir)
{
	if (!sgt || !sgt->sgl) return;
	linuxu_dma_unmap_sg(dev, sgt->sgl, sgt->nents, dir);
}
#endif

static uint32_t dart_hash(uint64_t iova)
{
	uint64_t h = iova ^ (iova >> 14) ^ (iova >> 32);
	return (uint32_t)h % DART_TABLE_BUCKETS;
}

/* Native mappings carry their own ownership records. Repeated aliases must
 * each refund exactly one charge, and unknown unmaps cannot refund another. */
static int dart_table_insert(uint64_t iova, uint64_t size, uint64_t length,
		enum dma_data_direction direction, bool coherent)
{
	struct dart_token *token = malloc(sizeof(*token));
	if (!token) return -ENOMEM;
	unsigned int bucket = dart_hash(iova);
	*token = (struct dart_token){ .next = dart_table[bucket], .iova = iova,
		.size = size, .length = length, .direction = direction, .coherent = coherent };
	dart_table[bucket] = token;
	dart_table_count++;
	return 0;
}
static uint64_t dart_table_remove(uint64_t iova, uint64_t length,
		enum dma_data_direction direction, bool coherent)
{
	struct dart_token **link = &dart_table[dart_hash(iova)];
	while (*link) {
		struct dart_token *token = *link;
		if (token->iova == iova && token->coherent == coherent &&
			(coherent || (token->length == length && token->direction == direction))) {
			uint64_t size = token->size;
			*link = token->next;
			free(token);
			dart_table_count--;
			return size;
		}
		link = &token->next;
	}
	return 0;
}

#ifndef LINUXU_DEXT_DK
/* Host: a held release keeps its token (the identity mapping stays live)
 * and, for a coherent buffer, its memory. */
struct dart_held {
	struct dart_held *next;
	uint64_t iova, length;
	enum dma_data_direction dir;
	void *coherent;		/* memory to free once released, or NULL */
};
static struct dart_held *dart_held_list;

/* Hold a release; false (release now) if the record cannot be made. */
static bool dart_hold_push(uint64_t iova, uint64_t length, enum dma_data_direction dir,
			   void *coherent)
{
	struct dart_held *h = malloc(sizeof(*h));

	if (!h)
		return false;
	*h = (struct dart_held){ .iova = iova, .length = length, .dir = dir,
				 .coherent = coherent };
	pthread_mutex_lock(&dart_lock);
	h->next = dart_held_list;
	dart_held_list = h;
	dart_held_count++;
	pthread_mutex_unlock(&dart_lock);
	return true;
}
#endif

static void dart_release_held_now(void)
{
#ifdef LINUXU_DEXT_DK
	for (;;) {
		struct dart_coherent *e = NULL;

		pthread_mutex_lock(&dart_lock);
		for (struct dart_coherent *it = dart_coherents; it; it = it->next) {
			if (it->held && !it->releasing && !it->aliases) {
				e = it;
				e->held = 0;
				e->releasing = 1;
				dart_held_count--;
				break;
			}
		}
		pthread_mutex_unlock(&dart_lock);
		if (!e)
			break;
		/* A failed completion keeps it, as for any free. */
		dart_coherent_release(e);
	}
	for (unsigned int i = 0;; ++i) {
		struct dart_stream entry, *st;

		pthread_mutex_lock(&dart_lock);
		if (i >= dart_stream_capacity) {
			pthread_mutex_unlock(&dart_lock);
			break;
		}
		st = dart_stream_at(i);
		if (!st->held) {
			pthread_mutex_unlock(&dart_lock);
			continue;
		}
		st->held = 0;
		dart_held_count--;
		entry = *st;
		pthread_mutex_unlock(&dart_lock);
		int result = dext_dma_free_coherent(entry.bounce, entry.charged);
		pthread_mutex_lock(&dart_lock);
		if (!result) {
			memset(st, 0, sizeof(*st));
			dart_refund(entry.charged);
			dart_table_count--;
		}
		pthread_mutex_unlock(&dart_lock);
	}
#else
	struct dart_held *list, *h;

	pthread_mutex_lock(&dart_lock);
	list = dart_held_list;
	dart_held_list = NULL;
	dart_held_count = 0;
	for (h = list; h; h = h->next) {
		uint64_t freed = dart_table_remove(h->iova, h->length, h->dir, h->coherent != NULL);

		dart_refund(freed);
		if (!freed)
			h->coherent = NULL;	/* not ours to free */
	}
	pthread_mutex_unlock(&dart_lock);
	while ((h = list)) {
		list = h->next;
		free(h->coherent);
		free(h);
	}
#endif
}

void linuxu_dart_set_hold(bool (*stalled)(void *), void *arg)
{
	pthread_mutex_lock(&dart_hold_lock);
	dart_hold_fn = stalled;
	dart_hold_arg = arg;
	pthread_mutex_unlock(&dart_hold_lock);
	dart_release_held_if_idle();
}

unsigned int linuxu_dart_held(void)
{
	unsigned int held;

	pthread_mutex_lock(&dart_lock);
	held = dart_held_count;
	pthread_mutex_unlock(&dart_lock);
	return held;
}

unsigned int linuxu_dart_release_held(void)
{
	dart_release_held_if_idle();
	return linuxu_dart_held();
}

/* ---- page map/unmap ---- */
dma_addr_t linuxu_dma_map_page(struct device *dev, struct page *page,
			       unsigned long offset, size_t size,
			       enum dma_data_direction dir)
{
	void *host;
	uint64_t iova;
	uint64_t charged;
	int rc;

	(void)dev; (void)dir;
	if (!page || size == 0 || !valid_dma_direction(dir))
		return (dma_addr_t)(uintptr_t)-22; /* -EINVAL */
	dart_release_held_if_idle();
	host = page_address(page);
	if (!host)
		return (dma_addr_t)(uintptr_t)-22; /* -EINVAL */
#ifdef LINUXU_DEXT_DK
	if (offset >= PAGE_SIZE || offset > UINTPTR_MAX - (uintptr_t)host)
		return (dma_addr_t)(uintptr_t)-EINVAL;
	return linuxu_dma_map_single(dev, (uint8_t *)host + offset, size, dir);
#else
	if (offset >= PAGE_SIZE || size > SIZE_MAX - offset ||
	    offset > UINTPTR_MAX - (uintptr_t)host || size > UINTPTR_MAX - ((uintptr_t)host + offset))
		return (dma_addr_t)(uintptr_t)-EINVAL;
	charged = offset + size;
	iova = (uint64_t)(uintptr_t)host + offset;

	pthread_mutex_lock(&dart_lock);
	rc = dart_table_insert(iova, charged, size, dir, false);
	if (!rc) dart_charge(charged);
	pthread_mutex_unlock(&dart_lock);
	if (rc)
		return (dma_addr_t)(uintptr_t)rc; /* -ENOMEM */
	return (dma_addr_t)iova;
#endif
}

void linuxu_dma_unmap_page(struct device *dev, dma_addr_t iova, size_t size,
			   enum dma_data_direction dir)
{
	uint64_t size_free;

	(void)dev; (void)dir;
	if (!iova || size == 0)
		return;
#ifdef LINUXU_DEXT_DK
	linuxu_dma_unmap_single(dev, iova, size, dir);
#else
	if (dart_hold_active() && dart_hold_push((uint64_t)iova, size, dir, NULL))
		return;
	pthread_mutex_lock(&dart_lock);
	size_free = dart_table_remove((uint64_t)iova, size, dir, false);
	dart_refund(size_free);
	pthread_mutex_unlock(&dart_lock);
#endif
}

/* ---- scatterlist map/unmap ---- */
int linuxu_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		      enum dma_data_direction dir)
{
	struct scatterlist *entry = sg;
	uint64_t total = 0;
	if (!sg || nents <= 0 || !valid_dma_direction(dir)) return 0;
	/* Validate the entire list before publishing any mappings. */
	for (int i = 0; i < nents; i++) {
		if (!entry || sg_is_chain(entry) || !entry->length) return 0;
		if (entry->page && (!page_address(entry->page) ||
				   entry->offset >= PAGE_SIZE)) return 0;
		if (entry->length > UINT64_MAX - total) return 0;
		total += entry->length;
		if (i + 1 < nents) entry = sg_next(entry);
	}
#ifdef LINUXU_DEXT_DK
	entry = sg;
	for (int i = 0; i < nents; i++) {
		void *source = entry->page ?
			(uint8_t *)page_address(entry->page) + entry->offset :
			(void *)(uintptr_t)entry->dma_address;
		dma_addr_t mapped = linuxu_dma_map_single(dev, source, entry->length, dir);
		if (dma_mapping_error(dev, mapped)) {
			struct scatterlist *prior = sg;
			for (int j = 0; j < i; j++) {
				dma_addr_t address = prior->dma_address;
				void *original = linuxu_dma_cpu_address(address);
				linuxu_dma_unmap_single(dev, address, prior->length, dir);
				prior->dma_address = prior->page ? 0 : (dma_addr_t)(uintptr_t)original;
				if (j + 1 < i) prior = sg_next(prior);
			}
			return 0;
		}
		entry->dma_address = mapped;
		if (i + 1 < nents) entry = sg_next(entry);
	}
#else
	pthread_mutex_lock(&dart_lock);
	dart_charge(total);
	entry = sg;
	for (int i = 0; i < nents; i++) {
		if (entry->page)
			entry->dma_address = (dma_addr_t)(uintptr_t)page_address(entry->page) + entry->offset;
		if (dart_table_insert((uint64_t)entry->dma_address, entry->length, entry->length, dir, false)) {
			struct scatterlist *prior = sg;
			for (int j = 0; j < i; j++) {
				dart_table_remove(prior->dma_address, prior->length, dir, false);
				prior = sg_next(prior);
			}
			dart_refund(total);
			pthread_mutex_unlock(&dart_lock);
			return 0;
		}
		if (i + 1 < nents) entry = sg_next(entry);
	}
	pthread_mutex_unlock(&dart_lock);
#endif
	return nents;
}

void linuxu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			 int nents, enum dma_data_direction dir)
{
	if (!sg || nents <= 0) return;
#ifndef LINUXU_DEXT_DK
	if (dart_hold_active()) {
		bool held = true;
		struct scatterlist *entry = sg;

		for (int i = 0; i < nents && entry && held; i++) {
			held = dart_hold_push((uint64_t)entry->dma_address, entry->length, dir, NULL);
			if (i + 1 < nents) entry = sg_next(entry);
		}
		if (held)
			return;
		/* Out of records: what was not held is released now. */
		dart_release_held_now();
	}
	pthread_mutex_lock(&dart_lock);
#endif
	for (int i = 0; i < nents && sg; i++) {
#ifdef LINUXU_DEXT_DK
		dma_addr_t address = sg->dma_address;
		void *original = linuxu_dma_cpu_address(address);
		linuxu_dma_unmap_single(dev, address, sg->length, dir);
		sg->dma_address = sg->page ? 0 : (dma_addr_t)(uintptr_t)original;
#else
		(void)dev; (void)dir;
		dart_refund(dart_table_remove((uint64_t)sg->dma_address, sg->length, dir, false));
#endif
		if (i + 1 < nents) sg = sg_next(sg);
	}
#ifndef LINUXU_DEXT_DK
	pthread_mutex_unlock(&dart_lock);
#endif
}

/* ---- coherent alloc/free ---- */
void *linuxu_dma_alloc_coherent(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp)
{
	uint64_t rounded, iova;
	void *p;
	int rc;
#ifdef LINUXU_DEXT_DK
	struct dart_coherent *coherent;
#endif

	(void)dev; (void)gfp;
	if (dma_handle) *dma_handle = DMA_MAPPING_ERROR;
	dart_release_held_if_idle();
	if (size == 0 || size > SIZE_MAX - (DART_COHERENT_ALIGN - 1))
		return NULL;
	rounded = (size + DART_COHERENT_ALIGN - 1) &
		  ~(DART_COHERENT_ALIGN - 1);
#ifdef LINUXU_DEXT_DK
	coherent = malloc(sizeof(*coherent));
	if (!coherent)
		return NULL;
	pthread_mutex_lock(&dart_lock);
	dart_charge(rounded);
	dart_allocations_inflight++;
	pthread_mutex_unlock(&dart_lock);
	/* driverKit: the real DART IOVA comes from the IODMACommand seam.
	 * The seam allocates the buffer (IOBufferMemoryDescriptor) and DMA-
	 * maps it; we get back the host pointer (p) AND the GPU-visible
	 * IOVA (iova). Accounting and bookkeeping are reserved before
	 * allocation; a refusal is reported by the seam, with its reason. */
	p = NULL;
	iova = 0;
	rc = dext_dma_alloc_coherent(rounded, &p, &iova);
	uint64_t mask = dev ? dev->coherent_dma_mask : DART_NO_LIMIT;
	if (dev && dev->bus_dma_limit && dev->bus_dma_limit < mask) mask = dev->bus_dma_limit;
	if ((gfp & __GFP_DMA32) && mask > UINT32_MAX) mask = UINT32_MAX;
	int invalid = rc != 0 || !p || !dart_iova_valid(iova, rounded) ||
		iova > mask || rounded - 1 > mask - iova;
	pthread_mutex_lock(&dart_lock);
	dart_allocations_inflight--;
	if (!p) {
		dart_refund(rounded);
		pthread_mutex_unlock(&dart_lock);
		free(coherent);
		return NULL;
	}
	*coherent = (struct dart_coherent){.cpu = p, .iova = iova,
		.size = rounded, .freeing = invalid, .releasing = invalid};
	coherent->next = dart_coherents;
	dart_coherents = coherent;
	dart_table_count++;
	pthread_mutex_unlock(&dart_lock);
	if (invalid) {
		dart_coherent_release(coherent);
		return NULL;
	}
	if (dma_handle) *dma_handle = (dma_addr_t)iova;
	return p;
#else
	p = aligned_alloc(DART_COHERENT_ALIGN, rounded);
	if (!p)
		return NULL;
	iova = (uint64_t)(uintptr_t)p;
	rc = 0;
#endif
	pthread_mutex_lock(&dart_lock);
	rc = dart_table_insert(iova, rounded, size, DMA_BIDIRECTIONAL, true);
	if (!rc) dart_charge(rounded);
	pthread_mutex_unlock(&dart_lock);
	if (rc) {
		free(p);
		return NULL;
	}
	if (dma_handle)
		*dma_handle = (dma_addr_t)iova;
	return p;
}

void linuxu_dma_free_coherent(struct device *dev, size_t size, void *vaddr,
			      dma_addr_t dma_handle)
{
	uint64_t rounded, size_free, iova;

	(void)dev;
	if (!vaddr)
		return;
	rounded = (size + DART_COHERENT_ALIGN - 1) &
		  ~(DART_COHERENT_ALIGN - 1);
	iova = (uint64_t)(uintptr_t)vaddr;
#ifdef LINUXU_DEXT_DK
	/* driverKit: the IOVA is the DMA handle the seam returned, not the
	 * host VA (they differ — the IOVA is a DART-assigned address). */
	iova = (uint64_t)(uintptr_t)dma_handle;
	pthread_mutex_lock(&dart_lock);
	struct dart_coherent *coherent = dart_coherent_by_cpu(vaddr, 1);
	if (!coherent || coherent->cpu != vaddr || coherent->iova != iova) {
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	coherent->freeing = 1;
	if (coherent->aliases || coherent->releasing) {
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	coherent->releasing = 1;
	pthread_mutex_unlock(&dart_lock);
	dart_coherent_retire(coherent);
	return;
#endif
#ifndef LINUXU_DEXT_DK
	if (iova == dma_handle && dart_hold_active() &&
	    dart_hold_push(iova, size, DMA_BIDIRECTIONAL, vaddr))
		return;
#endif
	pthread_mutex_lock(&dart_lock);
	size_free = iova == dma_handle ? dart_table_remove(iova, size, DMA_BIDIRECTIONAL, true) : 0;
	dart_refund(size_free);
	pthread_mutex_unlock(&dart_lock);
#ifdef LINUXU_DEXT_DK
	dext_dma_free_coherent(vaddr, rounded);
#else
	if (size_free) free(vaddr);
#endif
}

/* ---- imported mappings (rt/dart.h) ---- */
struct dart_import {
	struct dart_import *next;
	uint64_t iova, size;
};
static struct dart_import *dart_imports;	/* dart_lock */

int linuxu_dart_import(uint64_t iova, uint64_t size)
{
	struct dart_import *e;

	if (!iova || !size || size - 1 > UINT64_MAX - iova)
		return -EINVAL;
	e = malloc(sizeof(*e));
	if (!e)
		return -ENOMEM;
	*e = (struct dart_import){ .iova = iova, .size = size };
	pthread_mutex_lock(&dart_lock);
	for (const struct dart_import *o = dart_imports; o; o = o->next)
		if (iova < o->iova + o->size && o->iova < iova + size) {
			pthread_mutex_unlock(&dart_lock);
			free(e);
			return -EINVAL;
		}
	dart_charge(size);
	e->next = dart_imports;
	dart_imports = e;
	pthread_mutex_unlock(&dart_lock);
	return 0;
}

void linuxu_dart_import_release(uint64_t iova, uint64_t size)
{
	struct dart_import **link, *e = NULL;

	pthread_mutex_lock(&dart_lock);
	for (link = &dart_imports; *link; link = &(*link)->next)
		if ((*link)->iova == iova && (*link)->size == size) {
			e = *link;
			*link = e->next;
			dart_refund(size);
			break;
		}
	pthread_mutex_unlock(&dart_lock);
	free(e);
}

static int dart_import_live_locked(uint64_t iova, uint64_t bytes)
{
	for (const struct dart_import *e = dart_imports; e; e = e->next)
		if (iova >= e->iova && iova - e->iova < e->size && bytes <= e->size - (iova - e->iova))
			return 1;
	return 0;
}

/* ---- containment: what a device access may touch ---- */

#ifdef LINUXU_DEXT_DK
/* caller holds dart_lock */
static int dart_live_locked(uint64_t iova, uint64_t bytes)
{
	for (struct dart_coherent *e = dart_coherents; e; e = e->next)
		if (!e->releasing && iova >= e->iova && iova - e->iova < e->size &&
		    bytes <= e->size - (iova - e->iova))
			return 1;
	for (unsigned int i = 0; i < dart_stream_capacity; ++i) {
		const struct dart_stream *st = dart_stream_at(i);
		if (st->iova && (!st->releasing || st->held) && iova >= st->iova &&
		    iova - st->iova < st->charged && bytes <= st->charged - (iova - st->iova))
			return 1;
	}
	return 0;
}
#else
/* caller holds dart_lock */
static int dart_live_locked(uint64_t iova, uint64_t bytes)
{
	for (int b = 0; b < DART_TABLE_BUCKETS; ++b)
		for (const struct dart_token *t = dart_table[b]; t; t = t->next)
			if (iova >= t->iova && iova - t->iova < t->size &&
			    bytes <= t->size - (iova - t->iova))
				return 1;
	return 0;
}
#endif

int linuxu_dart_contains(uint64_t iova, uint64_t bytes)
{
	int live;

	if (!bytes || bytes - 1 > UINT64_MAX - iova)
		return 0;
	pthread_mutex_lock(&dart_lock);
	live = dart_live_locked(iova, bytes) || dart_import_live_locked(iova, bytes);
	pthread_mutex_unlock(&dart_lock);
	return live;
}

/* ---- getters / test hooks ---- */
uint64_t linuxu_dart_used(void)
{
	pthread_mutex_lock(&dart_lock);
	uint64_t v = dart_used;
	pthread_mutex_unlock(&dart_lock);
	return v;
}

uint64_t linuxu_dart_peak(void)
{
	pthread_mutex_lock(&dart_lock);
	uint64_t v = dart_peak;
	pthread_mutex_unlock(&dart_lock);
	return v;
}

int linuxu_dart_table_count(void)
{
	pthread_mutex_lock(&dart_lock);
	int n = dart_table_count;
	pthread_mutex_unlock(&dart_lock);
	return n;
}

void linuxu_dart_reset(void)
{
	pthread_mutex_lock(&dart_lock);
#ifdef LINUXU_DEXT_DK
	/* Reset is a test hook; live DriverKit mappings must be unmapped first. */
	if (dart_coherents || dart_allocations_inflight) {
		pthread_mutex_unlock(&dart_lock);
		return;
	}
	for (unsigned int i = 0; i < dart_stream_capacity; ++i)
		if (dart_stream_at(i)->iova || dart_stream_at(i)->releasing) {
			pthread_mutex_unlock(&dart_lock);
			return;
		}
#endif
	if (dart_table_count || dart_imports) { pthread_mutex_unlock(&dart_lock); return; }
	memset(dart_table, 0, sizeof(dart_table));
	dart_table_count = 0;
	dart_used = 0;
	dart_peak = 0;
	pthread_mutex_unlock(&dart_lock);
}
