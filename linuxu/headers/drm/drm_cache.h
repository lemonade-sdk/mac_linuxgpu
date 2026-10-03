/* linuxu: SHIM (third_party/linux/include/drm/drm_cache.h) — the
 * DRM cache-coherency helpers amdgpu_gmc.c uses
 * (drm_need_swiotlb, drm_clflush_virt_range, drm_memcpy_from_wc).
 * <linux/scatterlist.h> resolves to the linuxu shadow.  Only change:
 * drm_arch_can_wc_memory() is false on every linuxu build (see below). */
/*
 * Authors:
 * Dave Airlie <airlied@redhat.com>
 */

#ifndef _DRM_CACHE_H_
#define _DRM_CACHE_H_

#include <linux/scatterlist.h>

struct iosys_map;

void drm_clflush_pages(struct page *pages[], unsigned long num_pages);
void drm_clflush_sg(struct sg_table *st);
void drm_clflush_virt_range(void *addr, unsigned long length);
bool drm_need_swiotlb(int dma_bits);


/*
 * linuxu: always false.  Upstream returns false for CONFIG_ARM64, but the
 * shim config does not define it, so the generic branch returned true on
 * Apple Silicon.  That made amdgpu_bo_support_uswc() true: USWC GTT BOs got
 * ttm_write_combined and lost AMDGPU_PTE_SNOOPED in the GPU mapping, while
 * the CPU side of every GTT page (dext_dma_alloc_coherent) is cacheable.
 * Non-snooped GPU access to cacheable CPU memory is incoherent.  Upstream's
 * arm64 rationale applies unchanged:
 *
 * The DRM driver stack is designed to work with cache coherent devices
 * only, but permits an optimization to be enabled in some cases, where
 * for some buffers, both the CPU and the GPU use uncached mappings,
 * removing the need for DMA snooping and allocation in the CPU caches.
 *
 * The use of uncached GPU mappings relies on the correct implementation
 * of the PCIe NoSnoop TLP attribute by the platform, otherwise the
 * GPU will use cached mappings nonetheless. On x86 platforms, this does
 * not seem to matter, as uncached CPU mappings will snoop the caches in
 * any case. However, on ARM and arm64, enabling this optimization on a
 * platform where NoSnoop is ignored results in loss of coherency, which
 * breaks correct operation of the device. Since we have no way of
 * detecting whether NoSnoop works or not, just disable this
 * optimization entirely for ARM and arm64.
 */
static inline bool drm_arch_can_wc_memory(void)
{
	return false;
}

void drm_memcpy_init_early(void);

void drm_memcpy_from_wc(struct iosys_map *dst,
			const struct iosys_map *src,
			unsigned long len);
#endif
