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
 * A device that left the bus (rt/removal.h): imports, copies and checks
 * return -ENODEV; releasing still works, and the provider's release follows
 * as usual (the removal completes the GPU's fences).
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

/* Drop a reference; the last one unpins and drops the BO. The provider's
 * release follows once the BO is destroyed (possibly later, on a TTM
 * worker). */
void rt_surface_release(struct rt_surface *surface);

/* The surface's geometry. */
void rt_surface_geometry(const struct rt_surface *surface, uint32_t *width, uint32_t *height,
			 uint32_t *pitch);

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

/* ---- pinning check: does the GPU see what the client writes? ----
 * The client fills the surface with rt_surface_pattern(seed, i) for every
 * dword i; rt_surface_verify() reads RT_SURFACE_SAMPLES ranges spread over
 * the surface with the GPU (SDMA copies into a GTT buffer) and, when a CPU
 * view of the client's memory is given, with the CPU, and counts the
 * dwords that differ from the pattern of @seed. */
#define RT_SURFACE_SAMPLES	64u
#define RT_SURFACE_SAMPLE_BYTES	256u

static inline uint32_t rt_surface_pattern(uint32_t seed, uint64_t dword)
{
	return (uint32_t)(dword * 2654435761u) ^ (seed * 0x85ebca77u) ^ 0x5a5a0000u;
}

struct rt_surface_verify_result {
	uint32_t version;		/* 1 */
	uint32_t samples, sample_bytes;
	uint32_t gpu_mismatches;	/* dwords the GPU read that differ */
	uint32_t cpu_mismatches;	/* dwords the CPU view read that differ */
	uint32_t cpu_checked;		/* a CPU view was given */
	uint64_t first_gpu_mismatch;	/* surface byte offset, or UINT64_MAX */
	uint64_t first_cpu_mismatch;
	uint64_t gpu_ns;		/* the GPU read, submit to fence */
	uint64_t gpu_address;		/* the surface in the GART */
	uint32_t gpu_value, expected_value;	/* at the first GPU mismatch, or sample 0 */
};

/* Check the surface against the pattern of @seed. @cpu_view, when not NULL,
 * is a CPU mapping of the whole surface. Returns 0 (the counts tell the
 * result), or the GPU read's errno (-ETIME after @timeout_ms). */
int rt_surface_verify(struct rt_surface *surface, uint32_t seed, const void *cpu_view,
		      unsigned int timeout_ms, struct rt_surface_verify_result *result);

/* ---- imports by owner (a user client), by handle ---- */
#define RT_SURFACE_IMPORTS_MAX	32u

/* Register an imported surface for @owner: a nonzero handle, or 0 when the
 * table is full (the caller still owns @surface then). */
uint32_t rt_surface_add(uint64_t owner, struct rt_surface *surface);
/* The surface of @owner's @handle, or NULL. Valid until removed. */
struct rt_surface *rt_surface_get(uint64_t owner, uint32_t handle);
/* The provider context the surface was imported with. */
void *rt_surface_provider_context(const struct rt_surface *surface);
/* The surface of @owner's @handle with a hold the caller drops with
 * rt_surface_release(), or NULL. */
struct rt_surface *rt_surface_get_hold(uint64_t owner, uint32_t handle);
/* Another reference; rt_surface_release() drops one, the last releases. */
void rt_surface_hold(struct rt_surface *surface);

/* Remove and release: one handle (0 or -ENOENT), all of an owner's, or all
 * (both return how many). */
int rt_surface_remove(uint64_t owner, uint32_t handle);
unsigned int rt_surface_remove_owner(uint64_t owner);
unsigned int rt_surface_remove_all(void);
unsigned int rt_surface_count(void);

#ifdef __KERNEL__
struct drm_gem_object;
struct drm_sched_entity;
struct amdgpu_ring;
struct amdgpu_device;
struct dma_fence;

/* SDMA engines a caller owns for asynchronous copies: one scheduler
 * entity per ready SDMA ring, at most two. */
#define RT_SURFACE_ENGINES_MAX	2u
struct rt_surface_engines {
	unsigned int count;
	struct drm_sched_entity *entity[RT_SURFACE_ENGINES_MAX];
	struct amdgpu_ring *ring[RT_SURFACE_ENGINES_MAX];
};
int rt_surface_engines_init(struct amdgpu_device *adev, struct rt_surface_engines *engines);
void rt_surface_engines_fini(struct rt_surface_engines *engines);

/* Submit the copies of @count rectangles into @dst (an amdgpu BO the
 * caller keeps pinned in VRAM at @dst_address) and return without waiting.
 * Damage of 1 MiB or more is split between the engines. Each engine's last
 * fence is added to both buffers (a commit of @dst waits for it) and
 * returned in @fences (NULL for an engine left unused; the caller puts
 * them). Returns 0 or the failing step's errno (nothing submitted then). */
int rt_surface_copy_submit(struct rt_surface *src, struct drm_gem_object *dst, uint64_t dst_address,
			   uint32_t dst_pitch, const struct rt_surface_rect *rects, uint32_t count,
			   struct rt_surface_engines *engines,
			   struct dma_fence *fences[RT_SURFACE_ENGINES_MAX],
			   struct rt_surface_copy_stats *stats);

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
