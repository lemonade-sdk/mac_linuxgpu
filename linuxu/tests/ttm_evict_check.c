/* TTM moves between VRAM and GTT through the unmodified upstream code on the
 * CS fixture's software GPU (cs_fixture.h): ttm_bo_validate and eviction
 * under VRAM pressure run amdgpu_bo_move -> amdgpu_move_blit ->
 * amdgpu_ttm_copy_mem_to_mem, whose GTT side goes through a GART transfer
 * window: SDMA uploads the window's PTEs (built by amdgpu_gart_map from the
 * ttm_tt's DMA addresses) into the GART table, flushes VMID 0, then copies.
 *
 * The fixture's SDMA translates every access as the GPU does and checks
 * every system-memory page against the live DMA mappings, as the DART
 * would (a write outside them is the fault that can take a Thunderbolt
 * device off the bus). On top of that this checks the window PTEs SDMA
 * wrote: one per GPU page, valid, system, snooped for cached pages,
 * writable, pointing exactly at the ttm_tt's DMA addresses, each inside a
 * live mapping. The BAR is narrower than VRAM, as on the iPad, so an
 * invisible BO has no CPU-copy fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/dma-resv.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_tt.h>
#include <rt/dart.h>

#include "amdgpu.h"
#include "amdgpu_object.h"
#include "amdgpu_res_cursor.h"
#include "amdgpu_ttm.h"
#include "cs_fixture.h"

extern int amdgpu_gpu_recovery;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

/* One transfer window holds AMDGPU_GTT_MAX_TRANSFER_SIZE CPU pages, so a BO
 * this size leaves all its PTEs in the window after the move. */
#define EVICT_BO_BYTES	(8ULL << 20)

static struct amdgpu_device *adev;
static struct amdgpu_bo *staging;
static uint64_t staging_gpu;
static uint32_t *staging_cpu;

static uint32_t pattern(uint64_t offset, uint32_t seed)
{
	return (uint32_t)(offset * 2654435761u) ^ seed;
}

/* SDMA on the default entity between the staging (GART) and a reserved BO's
 * VRAM ranges, waited for (bounded). */
static void vram_copy(struct amdgpu_bo *bo, bool to_bo)
{
	const uint64_t vram = amdgpu_ttm_domain_start(adev, TTM_PL_VRAM);
	struct amdgpu_res_cursor cur;
	uint64_t done = 0;

	CHECK(bo->tbo.resource->mem_type == TTM_PL_VRAM);
	amdgpu_res_first(bo->tbo.resource, 0, amdgpu_bo_size(bo), &cur);
	while (cur.remaining) {
		struct dma_fence *fence = NULL;
		int r;

		mutex_lock(&adev->mman.default_entity.lock);
		r = amdgpu_copy_buffer(adev, &adev->mman.default_entity,
				       to_bo ? staging_gpu + done : vram + cur.start,
				       to_bo ? vram + cur.start : staging_gpu + done,
				       (uint32_t)cur.size, NULL, &fence, false, 0);
		mutex_unlock(&adev->mman.default_entity.lock);
		CHECK(!r && fence);
		CHECK(dma_fence_wait_timeout(fence, false, msecs_to_jiffies(5000)) > 0);
		dma_fence_put(fence);
		done += cur.size;
		amdgpu_res_next(&cur, cur.size);
	}
}

static struct amdgpu_bo *vram_bo_typed(uint64_t bytes, uint32_t seed, enum ttm_bo_type type);
static struct amdgpu_bo *vram_bo(uint64_t bytes, uint32_t seed)
{
	return vram_bo_typed(bytes, seed, ttm_bo_type_device);
}

static struct amdgpu_bo *vram_bo_typed(uint64_t bytes, uint32_t seed, enum ttm_bo_type type)
{
	struct amdgpu_bo_param bp = {
		.size = bytes,
		.byte_align = PAGE_SIZE,
		.bo_ptr_size = sizeof(struct amdgpu_bo),
		.domain = AMDGPU_GEM_DOMAIN_VRAM,
		.preferred_domain = AMDGPU_GEM_DOMAIN_VRAM,
		/* Like a KFD VRAM BO: device memory the CPU never maps. */
		.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
		.type = type,
	};
	struct amdgpu_bo *bo = NULL;

	CHECK(!amdgpu_bo_create(adev, &bp, &bo) && bo);
	if (seed) {
		CHECK(bytes <= EVICT_BO_BYTES);
		for (uint64_t i = 0; i < bytes / 4; ++i)
			staging_cpu[i] = pattern(i * 4, seed);
		CHECK(!amdgpu_bo_reserve(bo, false));
		vram_copy(bo, true);
		amdgpu_bo_unreserve(bo);
	}
	return bo;
}

/* The BO's pages after a move to GTT hold what it held in VRAM. */
static void check_tt_content(struct amdgpu_bo *bo, uint32_t seed)
{
	struct ttm_tt *ttm = bo->tbo.ttm;

	CHECK(bo->tbo.resource->mem_type == TTM_PL_TT);
	CHECK(ttm && ttm_tt_is_populated(ttm) && ttm->dma_address);
	for (uint64_t page = 0; page < ttm->num_pages; ++page) {
		const uint32_t *words = page_address(ttm->pages[page]);

		CHECK(words);
		for (uint64_t i = 0; i < PAGE_SIZE / 4; ++i)
			CHECK(words[i] == pattern(page * PAGE_SIZE + i * 4, seed));
	}
}

/* The PTEs the move's destination window left in the GART table: the move
 * entity whose window 1 maps this ttm_tt, entry for entry. */
static void check_window_ptes(struct amdgpu_bo *bo)
{
	const struct ttm_tt *ttm = bo->tbo.ttm;
	const uint64_t *table = cs_fixture_host_view(adev->gart.ptr);
	const uint64_t want = AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM | AMDGPU_PTE_READABLE |
			      AMDGPU_PTE_WRITEABLE |
			      (ttm->caching == ttm_cached ? AMDGPU_PTE_SNOOPED : 0);
	unsigned int found = 0;

	CHECK(ttm->num_pages <= AMDGPU_GTT_MAX_TRANSFER_SIZE);
	for (unsigned int e = 0; e < adev->mman.num_move_entities; ++e) {
		const uint64_t first = adev->mman.move_entities[e].gart_window_offs[1] >>
				       AMDGPU_GPU_PAGE_SHIFT;

		if ((table[first] & 0x0000fffffffff000ULL) != ttm->dma_address[0])
			continue;
		for (uint64_t page = 0; page < ttm->num_pages; ++page) {
			for (unsigned int j = 0; j < AMDGPU_GPU_PAGES_IN_CPU_PAGE; ++j) {
				const uint64_t entry = table[first + page * AMDGPU_GPU_PAGES_IN_CPU_PAGE + j];
				const uint64_t address = entry & 0x0000fffffffff000ULL;

				CHECK((entry & want) == want);
				CHECK(address == ttm->dma_address[page] + j * AMDGPU_GPU_PAGE_SIZE);
				CHECK(linuxu_dart_contains(address, AMDGPU_GPU_PAGE_SIZE));
			}
		}
		found++;
	}
	CHECK(found == 1);
}

static void wait_idle(struct amdgpu_bo *bo)
{
	CHECK(dma_resv_wait_timeout(bo->tbo.base.resv, DMA_RESV_USAGE_BOOKKEEP, false,
				    msecs_to_jiffies(5000)) > 0);
}

static int move_to(struct amdgpu_bo *bo, uint32_t domain)
{
	struct ttm_operation_ctx ctx = { .interruptible = false };
	int r;

	CHECK(!amdgpu_bo_reserve(bo, false));
	amdgpu_bo_placement_from_domain(bo, domain);
	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (!r)
		wait_idle(bo);
	amdgpu_bo_unreserve(bo);
	return r;
}

void ttm_evict_check(void)
{
	struct cs_fixture_stats before, after;
	struct amdgpu_bo *a, *b, *c;
	struct ttm_resource_manager *man;
	uint64_t free_bytes;

	adev = cs_fixture_adev();
	CHECK(adev->gmc.visible_vram_size < adev->gmc.real_vram_size);
	CHECK(!amdgpu_bo_create_kernel(adev, EVICT_BO_BYTES, PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
				       &staging, &staging_gpu, (void **)&staging_cpu));
	cs_fixture_stats(&before);

	/* 1. VRAM -> GTT by validation: the eviction path's move. */
	a = vram_bo(EVICT_BO_BYTES, 0xa5a5u);
	CHECK(!amdgpu_res_cpu_visible(adev, a->tbo.resource));	/* no CPU-copy fallback */
	CHECK(!move_to(a, AMDGPU_GEM_DOMAIN_GTT));
	check_tt_content(a, 0xa5a5u);
	check_window_ptes(a);

	/* 2. Back to VRAM: SDMA reads the pages through the source window. */
	CHECK(!move_to(a, AMDGPU_GEM_DOMAIN_VRAM));
	memset(staging_cpu, 0, EVICT_BO_BYTES);
	CHECK(!amdgpu_bo_reserve(a, false));
	vram_copy(a, false);
	amdgpu_bo_unreserve(a);
	for (uint64_t i = 0; i < EVICT_BO_BYTES / 4; ++i)
		CHECK(staging_cpu[i] == pattern(i * 4, 0xa5a5u));

	/* 3. Eviction under VRAM pressure: fill VRAM, then allocate one more
	 * BO; TTM evicts from the least recently used end (a first) to GTT. */
	man = &adev->mman.vram_mgr.manager;
	free_bytes = man->size - ttm_resource_manager_usage(man);
	CHECK(free_bytes > (32ULL << 20));
	b = vram_bo(free_bytes - (12ULL << 20), 0);
	c = vram_bo(16ULL << 20, 0);
	/* TTM picks its victims; a, the least recently used, is among them. */
	CHECK(a->tbo.resource->mem_type == TTM_PL_TT);
	CHECK(c->tbo.resource->mem_type == TTM_PL_VRAM);
	CHECK(b->tbo.resource->mem_type == TTM_PL_VRAM || b->tbo.resource->mem_type == TTM_PL_TT);
	wait_idle(a);
	CHECK(!amdgpu_bo_reserve(a, false));
	check_tt_content(a, 0xa5a5u);
	/* A later victim's move reuses the windows. */
	if (b->tbo.resource->mem_type == TTM_PL_VRAM)
		check_window_ptes(a);
	amdgpu_bo_unreserve(a);
	if (b->tbo.resource->mem_type == TTM_PL_TT)
		wait_idle(b);

	cs_fixture_stats(&after);
	CHECK(after.faults == before.faults && after.dart_faults == before.dart_faults);
	/* Window PTE uploads and the copies themselves went through SDMA. */
	CHECK(after.copies - before.copies >= 6 && after.vm_flushes > before.vm_flushes);

	/* 4. A kernel BO's move (a VM page table's, as TTM evicts them) whose
	 * SDMA work does not complete: amdgpu_move_blit waits for it
	 * (ttm_bo_wait_free_node, 15 s), the move fails ("Move buffer fallback
	 * to memcpy unavailable": the BO is beyond the BAR) and the BO stays in
	 * VRAM; TTM destroys the ttm_tt it had populated for the destination
	 * while the window PTE upload and the copy are still queued on SDMA,
	 * as on the iPad (ring sdma0 timeout, then the eviction failure).
	 * Without the DMA hold the copy, once the engine runs, writes pages
	 * that are no longer DMA-mapped: DART faults. */
	{
		const bool no_hold = getenv("CS_FIXTURE_NO_DMA_HOLD") != NULL;
		struct amdgpu_bo *k;
		ktime_t start;
		int r;

		amdgpu_bo_unref(&b);
		k = vram_bo_typed(EVICT_BO_BYTES, 0x5a5au, ttm_bo_type_kernel);
		cs_fixture_stats(&before);
		cs_fixture_hold_sdma(1);
		start = ktime_get();
		r = move_to(k, AMDGPU_GEM_DOMAIN_GTT);
		fprintf(stderr, "ttm evict: held-SDMA move of a kernel BO returned %d after %lld ms\n",
			r, (long long)ktime_ms_delta(ktime_get(), start));
		/* Within a bound. Without GPU recovery a clean failure: the BO
		 * is where it was. With it, the SDMA queue reset (10 s) ends
		 * the wait, its jobs cancelled (as Linux's per-queue reset
		 * cancels a guilty context's work), and TTM completes the move
		 * on their fences. */
		CHECK(amdgpu_gpu_recovery || (r && k->tbo.resource->mem_type == TTM_PL_VRAM));
		CHECK(ktime_ms_delta(ktime_get(), start) < 30000);
		/* With GPU recovery the SDMA job timed out first (10 s, before
		 * TTM's 15 s): its queue reset dropped the copy and the window
		 * PTE upload of the move's context, so nothing will write the
		 * pages TTM released. Without it, those pages are still mapped:
		 * the engine was stalled when they went. */
		if (amdgpu_gpu_recovery) {
			struct cs_fixture_stats now;

			cs_fixture_stats(&now);
			CHECK(now.queue_resets > before.queue_resets);
		} else {
			CHECK(no_hold || linuxu_dart_held() > 0);
		}
		/* The engine catches up and runs what was queued: its window
		 * PTEs point at those pages, which it may still write. */
		cs_fixture_hold_sdma(0);
		wait_idle(k);
		cs_fixture_stats(&after);
		if (no_hold) {
			/* CS_FIXTURE_NO_DMA_HOLD=1: the stray writes happen. */
			CHECK(after.dart_faults > before.dart_faults);
			puts("negative control: late SDMA work hit unmapped DMA addresses");
			exit(0);
		}
		CHECK(after.faults == before.faults && after.dart_faults == before.dart_faults);
		/* Nothing is stalled any more: the held releases go. */
		CHECK(linuxu_dart_release_held() == 0);
		amdgpu_bo_unref(&k);
		b = NULL;
	}

	amdgpu_bo_unref(&c);
	if (b)
		amdgpu_bo_unref(&b);
	amdgpu_bo_unref(&a);
	amdgpu_bo_free_kernel(&staging, &staging_gpu, (void **)&staging_cpu);
	printf("PASS TTM VRAM<->GTT moves offline: validation and eviction under VRAM pressure "
	       "through the GART transfer windows, window PTEs on live DMA mappings, "
	       "contents intact; a move whose SDMA work stalls ends within a bound (its "
	       "queue resets under GPU recovery); no GPU or DART "
	       "fault (BAR %llu MiB of %llu MiB VRAM)\n",
	       (unsigned long long)(adev->gmc.visible_vram_size >> 20),
	       (unsigned long long)(adev->gmc.real_vram_size >> 20));
}
