/* linuxu: SHIM (DeviceKit runtime glue) */
/*
 * rt/amdgpu_device_ext.h — the per-device glue object the KMD is handed.
 *
 * `struct amdgpu_device` (upstream amdgpu.h) owns `struct drm_device ddev;`
 * as its 3rd member.  The linuxu runtime layer needs a parallel,
 * KMD-visible place to stash the DriverKit state — the IOPCIDevice-backed
 * rt_device, the fake-MMIO token for the register BAR (BAR5), the BAR0
 * VRAM window, and the MSI-X vector table — without editing the upstream
 * struct.  This is that object: the dext main (or the drm_device glue)
 * allocates one `struct amdgpu_device_ext` per GPU, wires it to the
 * `struct amdgpu_device`, and the fake-MMIO / IRQ shims read it to route
 * their IOKit calls.
 *
 * The token fields here mirror the ABI documented in rt/rt.h:
 *   - mmio_token : u32 token for the register BAR (BAR5) fake-MMIO region.
 *   - vram_token : u32 token for the BAR0 VRAM fake-MMIO region.
 *   - *_base / *_size : the region base (GPU-visible physical) and size
 *                       the token was minted for.  Offsets the KMD hands
 *                       to rt_mmio_readl/writel are relative to these.
 */
#ifndef LINUXU_RT_AMDGPU_DEVICE_EXT_H
#define LINUXU_RT_AMDGPU_DEVICE_EXT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <rt/rt.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amdgpu_device;   /* upstream third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu.h — opaque here */
struct drm_device;      /* upstream drm core — opaque here */

/**
 * struct amdgpu_device_ext - per-device DeviceKit runtime glue
 *
 * @rt :        the rt_device (IOPCIDevice + MSI-X + DART budget owner).
 * @adev :      back-pointer to the upstream struct amdgpu_device.
 * @ddev :      back-pointer to the &struct drm_device embedded in @adev
 *              (== container_of(&adev->ddev) — kept explicit so the glue
 *              layer does not have to re-derive it).
 * @mmio_token :u32 fake-MMIO token for the register BAR (BAR5).  0 until
 *              rt_amdgpu_device_init has ioremapped it.
 * @mmio_base : GPU-visible physical base of the register BAR.
 * @mmio_size : size (bytes) of the register BAR mapping.
 * @vram_token :u32 fake-MMIO token for the BAR0 VRAM window.
 * @vram_base : GPU-visible physical base of VRAM.
 * @vram_size : total VRAM size (bytes) — may exceed the 256 MB window.
 * @vram_window: the CPU-visible BAR0 window (bytes) (<= 256 MB; ReBAR is
 *              not available under DriverKit).
 * @irq_vectors: number of MSI-X vectors in use (0 until IRQ init).
 * @initialized: true once rt_amdgpu_device_init has completed.
 */
struct amdgpu_device_ext {
	struct rt_device  *rt;
	struct amdgpu_device *adev;
	struct drm_device *ddev;

	uint32_t mmio_token;
	uint64_t mmio_base;
	uint64_t mmio_size;

	uint32_t vram_token;
	uint64_t vram_base;
	uint64_t vram_size;
	uint64_t vram_window;

	uint32_t irq_vectors;
	bool initialized;
};

/* ------------------------------------------------------------------ *
 * Lifecycle
 *
 * rt_amdgpu_device_alloc zeroes and links an ext to @adev.  The caller
 * still owns freeing it via rt_amdgpu_device_free.
 * ------------------------------------------------------------------ */
struct amdgpu_device_ext *rt_amdgpu_device_alloc(struct amdgpu_device *adev);
void rt_amdgpu_device_free(struct amdgpu_device_ext *dext);

/*
 * Open the IOPCIDevice, read BARs, mint the BAR5 (mmio_token) and BAR0
 * (vram_token) fake-MMIO tokens, and record the MSI-X vector count.
 * Returns 0 on success, negative errno on failure.  Fills in
 * mmio_base/size, vram_base/size/window, and irq_vectors.  Sets
 * @dext->initialized = true on success.
 */
int rt_amdgpu_device_init(struct amdgpu_device_ext *dext);

/* Quiesce, FLR, close PCI, release tokens, drop the rt_device. */
void rt_amdgpu_device_fini(struct amdgpu_device_ext *dext);

/* ------------------------------------------------------------------ *
 * Convenience register accessors.
 *
 * These are the "amdgpu_device_rreg/wreg" fast path: they skip the
 * pointer+token indirection and go straight at the BAR5 token with the
 * given register offset.  The RREG32/WREG32 macros in the KMD resolve to
 * these (via the linux/io.h shim) for the common `adev->rmmio` case.
 * ------------------------------------------------------------------ */
static inline uint32_t rt_amdgpu_rreg32(struct amdgpu_device_ext *dext,
					uint64_t off)
{
	return rt_mmio_readl(dext->rt, (void *)(uintptr_t)dext->mmio_token, off);
}
static inline void rt_amdgpu_wreg32(struct amdgpu_device_ext *dext,
				    uint64_t off, uint32_t v)
{
	rt_mmio_writel(dext->rt, (void *)(uintptr_t)dext->mmio_token, off, v);
}

/* ------------------------------------------------------------------ *
 * IRQ
 *
 * rt_amdgpu_irq_register wires the device's primary (IH) MSI-X vector
 * (vector 0 by default) to the amdgpu IRQ handler.  It is the
 * IOInterruptDispatchSource registration the amdgpu_irq.c path lands on.
 * Returns 0 on success.
 * ------------------------------------------------------------------ */
int rt_amdgpu_irq_register(struct amdgpu_device_ext *dext, int vector,
			   rt_irq_handler_t handler, const char *name,
			   void *arg);
void rt_amdgpu_irq_unregister(struct amdgpu_device_ext *dext, int vector);

/* Look up an ext from its amdgpu_device (NULL if none is attached). */
struct amdgpu_device_ext *rt_amdgpu_device_ext_get(struct amdgpu_device *adev);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_RT_AMDGPU_DEVICE_EXT_H */
