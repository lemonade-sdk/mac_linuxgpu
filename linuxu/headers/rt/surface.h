/* Another process's memory as a GPU buffer (linuxu/src/amdgpu-rt/surface.c):
 * a display agent's captured frame (an IOSurface), already mapped for the
 * device by the platform (the dext's IODMACommand over the client's memory
 * descriptor), is imported the way Linux imports any foreign buffer:
 *
 *   an exporter (dma_buf_export) whose sg_table carries the platform's DMA
 *   addresses, amdgpu_gem_prime_import() (an SG BO in GTT, its GART
 *   entries from that table), pinned and bound in the GART;
 *
 * then copied into a VRAM buffer (the scanout framebuffer) by the SDMA
 * buffer functions on the TTM entity: one copy packet per contiguous run
 * of the dirty rectangles, batched into jobs as amdgpu_copy_buffer() does.
 * The CPU touches no pixel.
 *
 * Lifetime: the platform mapping must stay valid until @release runs. It
 * runs once, when the dma-buf is released: after the BO was destroyed,
 * which TTM delays until every fence on it signalled, so no GPU access to
 * the memory can follow. It runs only for an import that succeeded; a
 * failed import leaves the mapping to the caller.
 *
 * No fallback: any failing step returns its errno; nothing is copied by the
 * CPU instead.
 *
 * Shared with DriverKit and C++ callers above the __KERNEL__ section. */
#ifndef LINUXU_RT_SURFACE_H
#define LINUXU_RT_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct rt_surface;

/* One contiguous run of device (DMA) addresses. Addresses and lengths are
 * multiples of the host page size. */
struct rt_surface_segment {
	uint64_t dma_address;
	uint64_t length;
};

struct rt_surface_provider {
	void (*release)(void *context);	/* the GPU can no longer access the memory */
	void *context;
};

#define RT_SURFACE_SEGMENTS_MAX	1024u

/* Import @count segments, @size bytes in all, laid out as a linear
 * @width x @height XRGB8888/BGRA surface with @pitch bytes per row. The
 * segments are charged to the DART budget (linuxu_dart_import) while
 * imported. Returns 0 and the surface, or -EINVAL, -ENOMEM, -ENODEV, or
 * the failing upstream step's errno. */
int rt_surface_import(struct pci_dev *pdev, const struct rt_surface_segment *segments,
		      uint32_t count, uint64_t size, uint32_t width, uint32_t height,
		      uint32_t pitch, const struct rt_surface_provider *provider,
		      struct rt_surface **out);

/* Drop the surface: unpin, drop the BO. The provider's release follows
 * once the BO is destroyed (possibly later, on a TTM worker). */
void rt_surface_release(struct rt_surface *surface);

/* The surface's GPU (GART) address while imported. */
uint64_t rt_surface_gpu_address(const struct rt_surface *surface);

struct rt_surface_rect {
	uint32_t x, y, width, height;
};

struct rt_surface_copy_stats {
	uint32_t jobs;		/* SDMA jobs submitted (up to 1024 copy packets each) */
	uint32_t rows;		/* rows covered */
	uint64_t bytes;
	uint64_t ns;		/* submit to fence signalled */
};

#ifdef __KERNEL__
struct drm_gem_object;

/* Copy @count rectangles of @src into @dst (an amdgpu BO, linear, @dst_pitch
 * bytes per row, at least the surface's size) at the same coordinates, on
 * the TTM buffer-function entity, and wait for the last copy's fence for at
 * most @timeout_ms. @dst is pinned in VRAM for the copy. Rectangles are
 * clipped to the surface; full-width runs with equal pitches are copied
 * as one range. Returns 0, -EINVAL, -ENODEV (no buffer functions), -ETIME,
 * or the failing step's errno. */
int rt_surface_copy(struct rt_surface *src, struct drm_gem_object *dst, uint32_t dst_pitch,
		    const struct rt_surface_rect *rects, uint32_t count, unsigned int timeout_ms,
		    struct rt_surface_copy_stats *stats);
#endif

#ifdef __cplusplus
}
#endif
#endif
