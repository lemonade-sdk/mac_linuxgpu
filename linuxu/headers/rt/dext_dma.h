/* linuxu: SHIM (DriverKit DART/DMA seam)
 *
 * rt/dext_dma.h — the real DART/DMA seam the linuxu DART layer (dart.c)
 * calls in the dext (DriverKit) build to get a GPU-visible IOVA over a
 * coherent buffer via the real DriverKit IODMACommand /
 * IOBufferMemoryDescriptor. Defined in dext/sources/iokit_bridge.mm
 * (#ifdef LINUXU_DEXT branch); the host build provides identity-IOVA
 * twins in the #else branch so a host unit test can exercise the same
 * shape without DriverKit.
 *
 * The 1.5 GB DART budget is OWNED by dart.c (the single source of truth);
 * the seam only converts (host VA <-> IOVA) and owns the IODMACommand /
 * buffer lifetime.  dart.c charges/refunds the budget around these calls.
 *
 * 16 KB host-page granularity: the allocated buffer is 16 KB aligned
 * (firmware/ucode loads need it).
 */
#ifndef LINUXU_RT_DEXT_DMA_H
#define LINUXU_RT_DEXT_DMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire the IOPCIDevice the DMA commands bind to (called alongside
 * dext_set_pci in dext_main.m, during the IOService Start).  0 on success. */
int dext_dma_set_pci(void *pci_device);

/* Stop new allocations and detach the PCI provider only when every DMA
 * descriptor has been released. Returns -1 if live mappings, operations,
 * or failed completion remain; the caller must retain the PCI provider.
 * Closing its PCI session to disable bus mastering is still permitted. */
int dext_dma_fini(void);
/* Reserve an idle provider against new DMA while a function reset runs. */
int dext_dma_begin_reset(void);
void dext_dma_end_reset(void);
/* Retire mappings without releasing DART/backing while the owner stops all
 * compute queues and upstream workers with IRQ delivery still available.
 * Pass linuxu_dart_budget(): retired mappings keep consuming this ceiling
 * even after the Linux owner refunds its logical allocation charge. */
int dext_dma_begin_shutdown(uint64_t dma_budget);
/* Cover allocations freed inside upstream's synchronous probe unwind before
 * control returns to the lifecycle owner. A failed probe promotes this hold
 * through begin_shutdown; commit releases only retired buffers on success. */
int dext_dma_begin_probe(uint64_t dma_budget);
int dext_dma_commit_probe(void);
/* Permanent retention, even if an in-flight RPC prevented orderly shutdown.
 * Stops new mappings, keeps future frees/aliases, and preserves the provider. */
void dext_dma_quarantine(void);
/* After software teardown and IRQ drain, reserve the endpoint for FLR.
 * All coherent mappings must be retired and all CPU aliases released. */
int dext_dma_begin_shutdown_reset(void);
/* Release retired descriptors only after verified BM-off FLR. Failure keeps
 * their backing pinned and blocks provider reuse. */
int dext_dma_end_shutdown_reset(int reset_succeeded);
/* After upstream removal, release BAR0 CPU mappings no owner can still use
 * (the aperture amdgpu leaves mapped after drm_dev_unplug()). Only under the
 * shutdown hold, with every DMA owner and other CPU alias already released.
 * Returns the number of references dropped, 0 if none, -1 if refused. */
int dext_bar0_cpu_release_orphaned(void);
/* 1 when a quarantined seam holds only retired descriptors and, possibly,
 * orphaned BAR0 CPU mappings (cached state). */
int dext_dma_quarantine_releasable(void);
/* Lift a quiescent quarantine back to the shutdown hold (keep_hold) so a
 * verified endpoint reset can release retired descriptors; without keep_hold
 * the table must be empty. 0 on success, -1 (unchanged) otherwise. */
int dext_dma_lift_quarantine(int keep_hold);

/* DMA addressing width, in bits, of the bound device: the effective width
 * of its Linux DMA masks (dma_mask.c publishes it from dma_set_mask and
 * dma_set_coherent_mask).  Every IODMACommand created afterwards uses it as
 * maxAddressBits, and a mapping the platform places at or above 2^bits is
 * refused instead of published.  Until a device sets a mask the width is
 * 64, leaving placement to the platform.  Accepts 32..64; returns 0, or -1
 * for a width outside that range (the current width is kept). */
int dext_dma_set_address_bits(unsigned int bits);
unsigned int dext_dma_address_bits(void);

/* Ask the platform's DMA translation (the DART behind IODMACommand)
 * whether it can place mappings entirely below 2^bits.  The answer comes
 * from the mapper itself: one 16 KiB buffer is prepared with that
 * maxAddressBits and its IOVA checked.  Answers are cached per provider.
 * Returns 1 (yes), 0 (no) or -1 (cannot tell now: no PCI provider, or the
 * probe failed for a reason unrelated to the width). */
int dext_dma_platform_supports_bits(unsigned int bits);

/* Allocate a coherent (DMA-mapped) buffer.  On success:
 *   *cpu_addr = the in-process host pointer (CPU reads/writes this)
 *   *iova     = the GPU-visible IOVA (segment.address from PrepareForDMA)
 * Returns 0 on success, -1 on any failure (the budget is the caller's
 * responsibility — dart.c charges it around the lifetime). */
int dext_dma_alloc_coherent(size_t size, void **cpu_addr, uint64_t *iova);

/* Free a coherent buffer from dext_dma_alloc_coherent.  cpu_addr is the
 * pointer that call returned. Returns 0 on successful teardown, -1 for
 * unknown/busy buffers or failed completion. On failed completion, the
 * seam retains the descriptor and backing; the caller must retain its
 * accounting and must not free or repurpose that storage. */
int dext_dma_free_coherent(void *cpu_addr, size_t size);

/* CPU-only descriptor backing, without a DMA command or DART reservation.
 * Pages allocated here can participate in real contiguous CPU aliases. */
void *dext_cpu_alloc_pages(size_t size);
int dext_cpu_free_pages(void *cpu_addr, size_t size);

/* CPU alias of live descriptor-backed pages; release the mapping with vunmap. */
void *dext_dma_vmap_pages(const void *const *pages, size_t count);
void dext_dma_vunmap_pages(const void *address);

/* Map the assigned visible BAR0 aperture into the dext address space.
 * Returns an actual CPU pointer or NULL; unmap releases one mapping claim. */
void *dext_bar0_cpu_map(uint64_t offset, uint64_t size);
int dext_bar0_cpu_unmap(const void *address);
int dext_bar0_cpu_contains(const void *address, size_t size);

/* Number of live coherent mappings (diagnostics for the test/bringup
 * harness; 0 at teardown = no leaks). */
int dext_dma_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_RT_DEXT_DMA_H */
