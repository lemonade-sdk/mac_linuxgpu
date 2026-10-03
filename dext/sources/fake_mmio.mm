/* linuxu shim: fake_mmio — dext-side fake-MMIO dispatch (dext build:
 * IOPCIDevice MemoryRead/Write; host build: shadow buffer).  Skeleton
 * with TODOs where DriverKit specifics need verification. */
#include <stdint.h>
#include <stdbool.h>

#ifdef LINUXU_DEXT

#import <DriverKit/IOService.h>
#import <PCIDriverKit/IOPCIDevice.h>

/* TODO(linuxu): the dext-side token table is the same ABI as the
 * host table (linuxu/src/pci/pdev_mmio.c); the dext build backs each
 * slot with (IOPCIDevice *, memIndex, base) instead of a shadow
 * buffer.
 *
 *   fake_mmio_init(IOPCIDevice *): capture the device
 *   fake_mmio_read32/64(token, off): MemoryRead32/64
 *   fake_mmio_write32/64(token, off, v): MemoryWrite32/64
 *   Offsets outside a token's BAR are rejected by dext_mem_*; upstream
 *   reaches registers beyond the register BAR through its own per-ASIC
 *   PCIE index/data pair.
 */

extern "C" int dext_mem_read32(uint32_t, uint64_t, uint32_t *);
extern "C" int dext_mem_read64(uint32_t, uint64_t, uint64_t *);
extern "C" int dext_mem_write32(uint32_t, uint64_t, uint32_t);
extern "C" int dext_mem_write64(uint32_t, uint64_t, uint64_t);

int fake_mmio_init(void *pci_device)
{
	return pci_device ? 0 : -1;
}

uint32_t fake_mmio_read32(uint32_t token, uint64_t off)
{
	uint32_t value = 0;
	(void)dext_mem_read32(token, off, &value);
	return value;
}

uint64_t fake_mmio_read64(uint32_t token, uint64_t off)
{
	uint64_t value = 0;
	(void)dext_mem_read64(token, off, &value);
	return value;
}

void fake_mmio_write32(uint32_t token, uint64_t off, uint32_t v)
{
	(void)dext_mem_write32(token, off, v);
}

void fake_mmio_write64(uint32_t token, uint64_t off, uint64_t v)
{
	(void)dext_mem_write64(token, off, v);
}

#else /* host build: forward to the shared shadow-buffer table */

extern uint32_t rt_mmio_readl(void *dev, void *p, uint64_t off);
extern uint64_t rt_mmio_readq(void *dev, void *p, uint64_t off);
extern void rt_mmio_writel(void *dev, void *p, uint64_t off, uint32_t v);
extern void rt_mmio_writeq(void *dev, void *p, uint64_t off, uint64_t v);

int fake_mmio_init(void *pci_device)
{
	(void)pci_device;
	return 0;
}

uint32_t fake_mmio_read32(uint32_t token, uint64_t off)
{
	return rt_mmio_readl(NULL, (void *)(uintptr_t)token, off);
}

uint64_t fake_mmio_read64(uint32_t token, uint64_t off)
{
	return rt_mmio_readq(NULL, (void *)(uintptr_t)token, off);
}

void fake_mmio_write32(uint32_t token, uint64_t off, uint32_t v)
{
	rt_mmio_writel(NULL, (void *)(uintptr_t)token, off, v);
}

void fake_mmio_write64(uint32_t token, uint64_t off, uint64_t v)
{
	rt_mmio_writeq(NULL, (void *)(uintptr_t)token, off, v);
}

#endif /* LINUXU_DEXT */
