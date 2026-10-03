/* linuxu: SHIM (DeviceKit runtime glue) */
/*
 * rt/rt.h — top-level header for the DeviceKit ("rt") runtime layer.
 *
 * This is the entry point everything in the KMD/dext talks to for the
 * hardware-facing primitives that do not exist in a normal kernel:
 * fake-MMIO register access, VRAM access, IRQ registration, DART buffers.
 *
 * The header is self-contained (no other linuxu headers required) so it can
 * be compiled standalone:
 *
 *     clang -std=gnu11 -fsyntax-only -D__KERNEL__ -Ilinuxu/headers \
 *         -x c - <<<'#include <rt/rt.h>'
 */
#ifndef LINUXU_RT_RT_H
#define LINUXU_RT_RT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <linux/irqreturn.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Fake-MMIO token ABI
 *
 * DriverKit cannot mmap a PCI BAR into the dext address space; every
 * register access ends at IOPCIDevice::MemoryRead32/64 / MemoryWrite32/64.
 * So `ioremap` does not return real memory — it returns a *token* that the
 * readl/writel family maps back onto a (memIndex, u64 offset) pair.
 *
 *   - A token is a u32.  token == 0 means "not a fake-MMIO pointer".
 *   - A token indexes a *slot* in the rt token table.
 *   - Slot granularity is 16 KB (one 16 KB host page) inside a 256 MB
 *     virtual region (token 1..16383 = slot 1..16383; 16384 slots).
 *   - The first word of the slot range is the 32-bit token itself, which
 *     is what an upstream driver obtains from pci_iomap() and later hands
 *     back to readl() etc.  The driver stores that u32 in a field sized
 *     for a pointer, so we transport the token in a pointer-sized union.
 *   - 32/64-bit accesses are at u64 *byte offsets* relative to the base
 *     of the region the token was minted for.  readb/writeb are at u8
 *     offsets (the driver only ever uses these for 1-byte config words).
 *   - Reads/writes dispatch to IOPCIDevice::MemoryRead32/64 /
 *     MemoryWrite32/64 for the slot's memIndex.  A token covers its BAR
 *     as assigned; offsets outside it are faults.  Registers beyond the
 *     register BAR are reached by upstream itself (adev->pcie_rreg/
 *     pcie_wreg through the per-ASIC PCIE index/data pair).
 *
 * The token itself is what a pointer-arithmetic-heavy driver expects:
 * `ioremap` gives you a `void *` whose value is the token, and `readl`
 * takes that `void *` and recovers the token.  Offsets are added by the
 * caller as plain integer arithmetic on the u64 offset they keep
 * separately (upstream keeps `base + off` in two variables, not in the
 * pointer), so we never actually do pointer arithmetic on the token.
 * ------------------------------------------------------------------ */

#define RT_MMIO_TOKEN_INVALID       0u
#define RT_MMIO_SLOT_GRANULE        0x4000u     /* 16 KB */
#define RT_MMIO_REGION_SIZE         0x10000000ull /* 256 MB */
#define RT_MMIO_NUM_SLOTS           (RT_MMIO_REGION_SIZE / RT_MMIO_SLOT_GRANULE)
/* DriverKit tokens are synthetic addresses, not allocated memory.  Give
 * each one enough address space for a full resizable VRAM BAR while keeping
 * the highest minted pointer below the arm64 user VA ceiling. */
#define RT_MMIO_DK_TOKEN_STRIDE     (1ull << 38) /* 256 GB */
#define RT_MMIO_DK_MAX_SLOTS        512u

/*
 * A fake-MMIO "pointer".  The driver stores the result of ioremap/pci_iomap
 * in a `void *` (or a field it treats as one) and passes it back to
 * readl()/writel()/readq()/writeq().  We transport the u32 token in the low
 * 32 bits of a u64 so pointer-sized storage is never overflowed, and so that
 * a NULL (0) ioremap result stays NULL.
 */
typedef struct {
	uint32_t token;        /* RT_MMIO_TOKEN_INVALID if not a fake mmio ptr */
	uint32_t _pad;         /* keep the struct 8 bytes / pointer-aligned */
} rt_mmio_ptr;

static inline rt_mmio_ptr rt_mmio_from_token(uint32_t token)
{
	rt_mmio_ptr p;
	p.token = token;
	p._pad = 0;
	return p;
}
static inline uint32_t rt_mmio_token(rt_mmio_ptr p) { return p.token; }
static inline bool rt_mmio_is_valid(rt_mmio_ptr p) { return p.token != RT_MMIO_TOKEN_INVALID; }

/* ------------------------------------------------------------------ *
 * Device handle
 *
 * The KMD is handed one `struct rt_device *` at boot (see
 * rt/amdgpu_device_ext.h).  It owns the IOPCIDevice, the MSI-X vectors,
 * the BARs, and the VRAM window.  All rt_* calls below take it as their
 * first argument so the shim can route to the right IOPCIDevice and so the
 * desktop (non-DriverKit) backend can substitute its own struct layout.
 * ------------------------------------------------------------------ */
struct rt_device;

/*
 * Bring up / tear down the whole runtime layer.  `rt_device_alloc` opens
 * the IOPCIDevice, reads the BARs, allocates the MSI-X vectors (up to 256),
 * and mints the initial tokens for the BAR regions the driver will ioremap.
 * It returns NULL on any IOKit failure.  `rt_device_free` is the three-phase
 * Stop/FinishStop equivalent: quiesce, FLR, close PCI, release resources.
 */
struct rt_device *rt_device_alloc(void);
void rt_device_free(struct rt_device *dev);

/* The rt device's fake-PCI handle (the adev's pdev).  NULL-safe; the
 * dext cold boot wires it into struct amdgpu_device.pdev. */
struct pci_dev *rt_device_get_pdev(struct rt_device *dev);
struct kobject *rt_device_kobject(struct rt_device *dev);
/* Live DriverKit PCI snapshot while the rt device exists; NULL otherwise. */
struct pci_dev *rt_device_active_pdev(void);
/* 0 when bound, the last probe error, -EAGAIN before probe, or
 * -EINPROGRESS while the callback is running. Registration alone is not
 * evidence that a PCI probe succeeded. */
int rt_pci_probe_result(struct pci_dev *dev);
/* True when failed-probe cleanup could not prove ownership quiescent.
 * Driver data, devres and module/device lifetime must remain retained, and
 * no new probe/removal may reuse this device during the process lifetime. */
int rt_pci_probe_cleanup_retained(struct pci_dev *dev);
/* Process-wide module shutdown gate; NULL device is not needed. */
int rt_pci_has_retained_probe(void);

/*
 * ioremap / pci_iomap equivalent.  `phys` is the BAR-relative physical
 * address (or, for BAR0 VRAM, the VRAM physical address); `size` is the
 * mapping length in bytes.  Returns a pointer whose low 32 bits are the
 * fresh token; RT_MMIO_TOKEN_INVALID (i.e. NULL) on failure.  The driver
 * later frees it with rt_mmio_free().
 */
void *rt_ioremap(struct rt_device *dev, uint64_t phys, uint64_t size);
void *rt_ioremap_active(uint64_t phys, uint64_t size);
void rt_mmio_free(void *cookie);

/*
 * The readl/writel/readq/writeq/readb/writeb signatures the driver's
 * kernel io.h shim calls.  `p` is the (token-bearing) pointer from
 * rt_ioremap; `off` is the u64 byte offset from the region base; the
 * read forms return the value, the write forms take it.  These are the
 * *runtime* implementations — the header-only io.h shim (owned by the
 * linux/ chunk) maps readl() etc. onto these.
 */
uint32_t rt_mmio_readl(struct rt_device *dev, void *p, uint64_t off);
void     rt_mmio_writel(struct rt_device *dev, void *p, uint64_t off, uint32_t v);
uint64_t rt_mmio_readq(struct rt_device *dev, void *p, uint64_t off);
void     rt_mmio_writeq(struct rt_device *dev, void *p, uint64_t off, uint64_t v);
uint8_t  rt_mmio_readb(struct rt_device *dev, void *p, uint64_t off);
void     rt_mmio_writeb(struct rt_device *dev, void *p, uint64_t off, uint8_t v);

/*
 * Bulk copies over the register/VRAM window.  `rt_mmio_memcpy_fromio`
 * copies from device to CPU; `rt_mmio_memcpy_toio` the reverse.  For VRAM
 * (BAR0) these use the proven bar0_memcpy_to_vram / read helpers rather
 * than a per-word loop.  `off` is the region-relative byte offset, `len`
 * the byte count (must be a multiple of the access width; the layer
 * handles unaligned tails internally).
 */
void rt_mmio_memcpy_fromio(void *dst, void *p, uint64_t off, size_t len,
			   struct rt_device *dev);
void rt_mmio_memcpy_toio(void *p, const void *src, uint64_t off, size_t len,
			 struct rt_device *dev);

/* ------------------------------------------------------------------ *
 * VRAM (BAR0) access
 *
 * The BAR0 window is 256 MB visible; the rest of VRAM is GPU-only and is
 * reached via SDMA staging (a concern of the KMD, not this layer).  These
 * two are the BAR0-window read/write over the same token machinery as
 * above but with a dedicated VRAM memIndex so the desktop backend can
 * service them from a plain buffer instead of IOPCIDevice.
 * ------------------------------------------------------------------ */
uint64_t rt_vram_base(struct rt_device *dev);      /* GPU-visible base addr */
uint64_t rt_vram_size(struct rt_device *dev);      /* total VRAM bytes      */
uint64_t rt_vram_window(struct rt_device *dev);    /* CPU-visible BAR0 bytes */

/* ------------------------------------------------------------------ *
 * IRQ
 *
 * rt_irq_register wires an IOInterruptDispatchSource on one of the
 * MSI-X vectors to `handler`.  `vector` is the MSI-X vector number
 * (0-based, < 256).  `arg` is passed to the handler.  Returns 0 on
 * success.  The handler runs on the dext's irq queue; it must report
 * in_interrupt()==1 (the linuxu TLS flag) and must not take a
 * fence-wait.  rt_irq_unregister tears the source down and (after a
 * drain) frees the vector.
 * ------------------------------------------------------------------ */
typedef int (*rt_irq_handler_t)(int irq, void *arg);

int  rt_irq_register(struct rt_device *dev, int vector, rt_irq_handler_t handler,
		     const char *name, void *arg);
void rt_irq_unregister(struct rt_device *dev, int vector);
int rt_irq_disable(unsigned int vector, bool synchronize);
void rt_irq_enable(unsigned int vector);
void rt_irq_synchronize(unsigned int vector);

/* Number of MSI-X vectors the device actually exposes (<= 256). */
int rt_irq_vector_count(struct rt_device *dev);

/* ------------------------------------------------------------------ *
 * DART (device DMA) buffers — the "real pointer" side
 *
 * In contrast to BAR MMIO, DART buffers are ordinary in-process pointers
 * (the cpu address of an IOBufferMemoryDescriptor).  These alloc/free
 * against the ~1.5 GB DART budget and return both the cpu address and the
 * device (IOVA) address the GPU will use in page tables.  The budget is
 * enforced by the layer; allocation fails loudly (returns NULL / 0) at
 * the ceiling rather than silently over-committing.
 * ------------------------------------------------------------------ */
void *rt_dart_alloc(struct rt_device *dev, size_t size,
		    uint64_t *dma_addr);
void  rt_dart_free(struct rt_device *dev, void *cpu, size_t size,
		   uint64_t dma_addr);

/* Current DART budget usage / ceiling, in bytes. */
uint64_t rt_dart_used(struct rt_device *dev);
uint64_t rt_dart_ceiling(struct rt_device *dev);

/* ---- PCI/IRQ (WP T1 host backend) ----
 * rt_pci_irq_* is the shim-internal seam between pci_stub.c's
 * request_irq/free_irq and rt_irq_register/rt_irq_unregister (the
 * rt-side implementation lives in amdgpu-rt/irq.c).  Declared here so
 * link units outside the amdgpu-rt build set (tests) can consume the
 * seam without dragging in amdgpu headers. */
struct pci_dev;
/* Kernel irq handler, same representation as
 * <linux/interrupt.h>'s irq_handler_t (both are
 * irqreturn_t (*)(int, void*) / int (*)(int, void*)). */
typedef irqreturn_t (*rt_irq_handler_fn)(int irq, void *dev_id);
int rt_pci_irq_request(struct pci_dev *dev, unsigned int irq,
		       rt_irq_handler_fn handler, unsigned long flags,
		       const char *name, void *dev_id);
void rt_pci_irq_free(struct pci_dev *dev, unsigned int irq,
		     void *dev_id);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_RT_RT_H */
