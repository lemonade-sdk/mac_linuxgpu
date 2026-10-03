/* linuxu: SHIM (third_party/linux/include/linux/io.h) - MMIO over driverkit window
 *
 * Host-backend readl/writel family: pci_iomap() hands out a fake-MMIO
 * TOKEN (a small u32, see rt/rt.h), not a mapped address — so plain
 * pointer dereferences would fault.  When the build has a host shadow
 * (LINUXU_RT_HOST_SHADOW, set by the host Makefile targets), the
 * accessors resolve the token's shadow buffer and dereference it
 * directly (the shadow is a real host buffer, so plain stores work and
 * are visible to the unit tests).  Without the define (dext backend) the
 * old plain-deref behavior is kept: the dext services accesses through
 * IOPCIDevice via the rt_mmio_* dispatch, not through these inlines. */
#ifndef _LINUX_IO_H
#define _LINUX_IO_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/memremap.h>
#include <stdlib.h>

#ifndef __iomem
#define __iomem
#endif

#include <rt/rt.h>

#if defined(LINUXU_DEXT_DK)
#include <rt/dext_dma.h>
/* rt/dext_pci.h: the device left the bus. Its BAR0 aperture is then never
 * touched again: reads see ~0, as a device gone from PCIe answers, and
 * writes are dropped. */
extern int dext_pci_removed(void);
/* Each token owns a 256 GB synthetic address range.  Pointer arithmetic
 * within a BAR preserves the token and yields a BAR-relative byte offset. */
static inline void *linuxu_dk_token(const void __iomem *addr)
{
	return (void *)(uintptr_t)((uintptr_t)addr / RT_MMIO_DK_TOKEN_STRIDE);
}
static inline uint64_t linuxu_dk_offset(const void __iomem *addr)
{
	return (uintptr_t)addr % RT_MMIO_DK_TOKEN_STRIDE;
}
extern uint16_t rt_mmio_readw(struct rt_device *, void *, uint64_t);
extern void rt_mmio_writew(struct rt_device *, void *, uint64_t, uint16_t);
/* Direct BAR0 CPU-mapping accesses follow Linux arm64 ordering: a write
 * barrier before each store, so descriptors and ring contents written to
 * DMA memory are visible to the GPU before the register/doorbell write;
 * a read barrier after each load, so later reads of DMA memory cannot be
 * satisfied before the register read that reported completion.  Accesses
 * of the natural width are single, untorn loads/stores.  The token path is
 * an RPC whose call boundary (and the dext's doorbell fence) already
 * orders it, so it takes no extra barrier here.  No _relaxed variants
 * exist yet; they would omit both barriers. */
#define linuxu_bar0_read(type, addr) ({				\
	type __v = (type)~(type)0;					\
	if (!dext_pci_removed()) {					\
		__v = *(const volatile type *)(addr);			\
		dma_rmb();						\
	}								\
	__v;								\
})
#define linuxu_bar0_write(type, v, addr) do {			\
	if (!dext_pci_removed()) {					\
		dma_wmb();						\
		*(volatile type *)(addr) = (v);				\
	}								\
} while (0)
static inline u8 readb(const void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u8)))
		return linuxu_bar0_read(u8, addr);
	return rt_mmio_readb(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr));
}
static inline u16 readw(const void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u16)))
		return linuxu_bar0_read(u16, addr);
	return rt_mmio_readw(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr));
}
static inline u32 readl(const void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u32)))
		return linuxu_bar0_read(u32, addr);
	return rt_mmio_readl(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr));
}
static inline u64 readq(const void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u64)))
		return linuxu_bar0_read(u64, addr);
	return rt_mmio_readq(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr));
}
static inline void writeb(u8 v, void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u8))) {
		linuxu_bar0_write(u8, v, addr);
		return;
	}
	rt_mmio_writeb(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr), v);
}
static inline void writew(u16 v, void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u16))) {
		linuxu_bar0_write(u16, v, addr);
		return;
	}
	rt_mmio_writew(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr), v);
}
static inline void writel(u32 v, void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u32))) {
		linuxu_bar0_write(u32, v, addr);
		return;
	}
	rt_mmio_writel(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr), v);
}
static inline void writeq(u64 v, void __iomem *addr)
{
	if (dext_bar0_cpu_contains(addr, sizeof(u64))) {
		linuxu_bar0_write(u64, v, addr);
		return;
	}
	rt_mmio_writeq(NULL, linuxu_dk_token(addr), linuxu_dk_offset(addr), v);
}
#elif !defined(LINUXU_RT_HOST_SHADOW)
static inline u8 readb(const void __iomem *addr)
{
	return *(const u8 __iomem *)addr;
}
static inline u16 readw(const void __iomem *addr)
{
	return *(const u16 __iomem *)addr;
}
static inline u32 readl(const void __iomem *addr)
{
	return *(const u32 __iomem *)addr;
}
static inline u64 readq(const void __iomem *addr)
{
	return *(const u64 __iomem *)addr;
}
static inline void writeb(u8 v, void __iomem *addr)
{
	*(u8 __iomem *)addr = v;
}
static inline void writew(u16 v, void __iomem *addr)
{
	*(u16 __iomem *)addr = v;
}
static inline void writel(u32 v, void __iomem *addr)
{
	*(u32 __iomem *)addr = v;
}
static inline void writeq(u64 v, void __iomem *addr)
{
	*(u64 __iomem *)addr = v;
}
#else
/* host shadow: token -> slot shadow buffer -> real host memory.
 * struct mmio_slot is defined in pdev_mmio.c; the inlines only touch
 * ->shadow, so mirror the shape here (single source: keep in sync).
 * pdev_mmio.c includes this header BEFORE its own definition, so the
 * #ifndef guard makes the two definitions the same one. */
#ifndef _LINUXU_MMIO_SLOT_DEFINED
#define _LINUXU_MMIO_SLOT_DEFINED
struct mmio_slot {
	int used;
	void *dev;
	uint8_t mem_index;
	uint64_t base;
	uint64_t size;
	uint8_t *shadow;
	size_t shadow_len;
};
#endif /* _LINUXU_MMIO_SLOT_DEFINED */
extern const struct mmio_slot *rt_mmio_slot_lookup(uint32_t token, size_t *off);

static inline u8 readb(const void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	return s ? s->shadow[off] : 0;
}
static inline u16 readw(const void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	return s ? *(const u16 *)(s->shadow + off) : 0;
}
static inline u32 readl(const void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	return s ? *(const u32 *)(s->shadow + off) : 0;
}
static inline u64 readq(const void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	return s ? *(const u64 *)(s->shadow + off) : 0;
}
static inline void writeb(u8 v, void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	if (s)
		s->shadow[off] = v;
}
static inline void writew(u16 v, void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	if (s)
		*(u16 *)(s->shadow + off) = v;
}
static inline void writel(u32 v, void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	if (s)
		*(u32 *)(s->shadow + off) = v;
}
static inline void writeq(u64 v, void __iomem *addr)
{
	uint32_t token = (uintptr_t)addr;
	size_t off = (uintptr_t)addr - (uintptr_t)token;
	const struct mmio_slot *s = rt_mmio_slot_lookup(token, &off);
	if (s)
		*(u64 *)(s->shadow + off) = v;
}
#endif /* LINUXU_RT_HOST_SHADOW */

static inline void __iomem *ioport_map(unsigned long port, unsigned int size)
{
	return (void __iomem *)port;
}
static inline void ioport_unmap(void __iomem *addr)
{
}

static inline void memcpy_fromio(void *to, const void __iomem *from, size_t count)
{
#ifdef LINUXU_DEXT_DK
	if (dext_bar0_cpu_contains(from, count)) {
		for (size_t i = 0; i < count; ++i)
			((u8 *)to)[i] = dext_pci_removed() ? 0xff : ((const volatile u8 *)from)[i];
		return;
	}
	rt_mmio_memcpy_fromio(to, linuxu_dk_token(from), linuxu_dk_offset(from), count, NULL);
#else
	__builtin_memcpy(to, from, count);
#endif
}
static inline void memcpy_toio(void __iomem *to, const void *from, size_t count)
{
#ifdef LINUXU_DEXT_DK
	if (dext_bar0_cpu_contains(to, count)) {
		for (size_t i = 0; i < count && !dext_pci_removed(); ++i)
			((volatile u8 *)to)[i] = ((const u8 *)from)[i];
		return;
	}
	rt_mmio_memcpy_toio(linuxu_dk_token(to), from, linuxu_dk_offset(to), count, NULL);
#else
	__builtin_memcpy(to, from, count);
#endif
}

static inline void ioread8_iter(const void __iomem *addr, u8 *val)
{
	*val = readb(addr);
}
static inline void iowrite8_iter(u8 val, void __iomem *addr)
{
	writeb(val, addr);
}
static inline void ioread32rep(void __iomem *addr, void *buf, unsigned int count)
{
	memcpy_fromio(buf, addr, count * 4);
}
static inline void iowrite32rep(void __iomem *addr, u32 val, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++)
		writel(val, (void __iomem *)((uintptr_t)addr + i * 4));
}

/* Map an assigned PCI BAR range into the synthetic DriverKit token space. */
static inline void __iomem *ioremap(phys_addr_t addr, unsigned long size)
{
#ifdef LINUXU_DEXT_DK
	return rt_ioremap_active(addr, size);
#else
	(void)addr; (void)size;
	return NULL;
#endif
}
static inline void __iomem *ioremap_wc(phys_addr_t addr, unsigned long size)
{
#ifdef LINUXU_DEXT_DK
	return rt_ioremap_active(addr, size);
#else
	(void)addr; (void)size;
	return NULL;
#endif
}
static inline void __iomem *ioremap_cache(phys_addr_t addr, unsigned long size)
{
	return ioremap(addr, size);
}


static inline void iounmap(const volatile void __iomem *addr)
{
#ifdef LINUXU_DEXT_DK
	if (addr) rt_mmio_free((void *)(uintptr_t)addr);
#else
	(void)addr;
#endif
}


static inline void *memset_io(void __iomem *addr, int val, size_t count)
{
#ifdef LINUXU_DEXT_DK
	if (dext_bar0_cpu_contains(addr, count)) {
		for (size_t i = 0; i < count && !dext_pci_removed(); ++i)
			((volatile u8 *)addr)[i] = (u8)val;
		return (void *)addr;
	}
	for (size_t i = 0; i < count; i++)
		writeb((u8)val, (void __iomem *)((uintptr_t)addr + i));
	return (void *)addr;
#else
	return memset((void *)addr, val, count);
#endif
}

/* memremap callers use ordinary pointer accesses. Only a real CPU mapping
 * is valid; a synthetic MMIO token or unrelated heap allocation is not. */
static inline void __iomem *memremap(phys_addr_t start, size_t size, unsigned long flags)
{
	if (!size || !(flags & (MEMREMAP_WB | MEMREMAP_WT | MEMREMAP_WC)) ||
	    (flags & ~(MEMREMAP_WB | MEMREMAP_WT | MEMREMAP_WC)))
		return NULL;
#ifdef LINUXU_DEXT_DK
	void *address = rt_ioremap_active(start, size);
	if (address && !dext_bar0_cpu_contains(address, size)) {
		rt_mmio_free(address);
		return NULL;
	}
	return address;
#else
	(void)start;
	return NULL;
#endif
}
static inline void memunmap(void __iomem *addr)
{
	iounmap(addr);
}



/** WC phys/io memtype tracking (vendor 2026 io.h no-op defaults) */
#ifndef arch_phys_wc_add
static inline int __must_check arch_phys_wc_add(unsigned long base,
	unsigned long size)
{
	return 0;
}
	#define arch_phys_wc_add arch_phys_wc_add
#endif

#ifndef arch_phys_wc_del
static inline void arch_phys_wc_del(int handle)
{
}
	#define arch_phys_wc_del arch_phys_wc_del
#endif

#ifndef arch_io_reserve_memtype_wc
static inline int arch_io_reserve_memtype_wc(unsigned long base,
	unsigned long size)
{
	return 0;
}
#endif

#ifndef arch_io_free_memtype_wc
static inline void arch_io_free_memtype_wc(unsigned long base,
	unsigned long size)
{
}
#endif

#endif /* _LINUX_IO_H */
