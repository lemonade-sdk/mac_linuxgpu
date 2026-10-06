/* A kernel mapping of VRAM is a pointer into the CPU-visible window, or no
 * mapping at all (patches/linux/amdgpu-ttm-visible-kmap.patch). The BO the
 * grid-penalty experiment of build 258.3 made for an MQD, contiguous VRAM
 * without AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED as
 * amdgpu_amdkfd_alloc_kernel_mem makes it, placed past the window: its kmap
 * pointed past the BAR0 mapping, the dext stored through it as process
 * memory and died, and the dext's death with its PCI session open panicked
 * the Mac. Its kmap must now fail; a CPU-visible BO still maps where it did.
 * The fixture's BAR is a quarter of VRAM (test_cs_selftest.c). */
#include <stdio.h>
#include <stdlib.h>

#include <drm/ttm/ttm_bo.h>

#include "amdgpu.h"
#include "amdgpu_object.h"
#include "amdgpu_ttm.h"
#include "cs_fixture.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

#define KMAP_BO_BYTES	(1ULL << 20)

void visible_kmap_check(void)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	const uint64_t visible = adev->gmc.visible_vram_size;
	struct ttm_operation_ctx ctx = { .interruptible = false, .no_wait_gpu = false };
	struct amdgpu_bo_param bp = {
		.size = KMAP_BO_BYTES,
		.byte_align = PAGE_SIZE,
		.domain = AMDGPU_GEM_DOMAIN_VRAM,
		.flags = AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS | AMDGPU_GEM_CREATE_CPU_GTT_USWC,
		.type = ttm_bo_type_kernel,
		.bo_ptr_size = sizeof(struct amdgpu_bo),
	};
	struct amdgpu_bo *bo = NULL;
	void *cpu = NULL;
	uint64_t gpu = 0;
	int r;

	CHECK(visible < adev->gmc.real_vram_size);

	/* Past the window. */
	CHECK(amdgpu_bo_create(adev, &bp, &bo) == 0);
	CHECK(amdgpu_bo_reserve(bo, true) == 0);
	amdgpu_bo_placement_from_domain(bo, AMDGPU_GEM_DOMAIN_VRAM);
	bo->placements[0].fpfn = visible >> PAGE_SHIFT;
	bo->placements[0].lpfn = 0;
	CHECK(ttm_bo_validate(&bo->tbo, &bo->placement, &ctx) == 0);
	CHECK(bo->tbo.resource->mem_type == TTM_PL_VRAM);
	CHECK(bo->tbo.resource->placement & TTM_PL_FLAG_CONTIGUOUS);
	CHECK(((uint64_t)bo->tbo.resource->start << PAGE_SHIFT) >= visible);
	CHECK(!amdgpu_res_cpu_visible(adev, bo->tbo.resource));
	r = amdgpu_bo_kmap(bo, &cpu);
	if (r == 0)
		fprintf(stderr, "kmap of VRAM at %#llx (window %#llx) gave %p, aper_base_kaddr %p\n",
			(unsigned long long)bo->tbo.resource->start << PAGE_SHIFT,
			(unsigned long long)visible, cpu, adev->mman.aper_base_kaddr);
	CHECK(r != 0 && !cpu && !amdgpu_bo_kptr(bo));
	amdgpu_bo_unreserve(bo);
	amdgpu_bo_unref(&bo);

	/* Inside it (CPU_ACCESS_REQUIRED, as amdgpu_bo_create_kernel asks):
	 * aper_base_kaddr plus the offset, every byte inside the window. */
	CHECK(amdgpu_bo_create_kernel(adev, KMAP_BO_BYTES, PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM,
				      &bo, &gpu, &cpu) == 0);
	CHECK(bo && cpu);
	CHECK(amdgpu_res_cpu_visible(adev, bo->tbo.resource));
	CHECK((uint8_t *)cpu == (uint8_t *)adev->mman.aper_base_kaddr +
	      ((uint64_t)bo->tbo.resource->start << PAGE_SHIFT));
	CHECK(((uint64_t)bo->tbo.resource->start << PAGE_SHIFT) + KMAP_BO_BYTES <= visible);
	amdgpu_bo_free_kernel(&bo, &gpu, &cpu);
	printf("PASS visible kmap: contiguous VRAM past the %llu MiB window has no kernel mapping; "
	       "CPU-visible VRAM maps inside it\n", (unsigned long long)(visible >> 20));
}
