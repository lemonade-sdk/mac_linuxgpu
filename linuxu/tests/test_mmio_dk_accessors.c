/* linux/io.h's DriverKit accessors decide by address: the BAR0 aperture
 * goes through the kernel, the token range (rt/rt.h, from 2^48, where no
 * process memory can be) goes to MMIO, a stale or never-minted token
 * faults, and anything else is process memory, read and written as Linux
 * does. Build 249 decoded any address by dividing it by the token stride:
 * an IB at 0x127849020, into which amdgpu_gmc_set_pte_pde writes the GART
 * window's PTEs with writeq during an SDMA eviction copy, decoded as token
 * 0, recorded a definite MMIO fault and closed PCI admission ("device lost
 * from bus" with the GPU still on it). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <rt/rt.h>
#include <rt/dext_pci.h>

extern uint32_t rt_mmio_mint_token(struct pci_dev *, uint8_t, uint64_t, uint64_t, int);
extern void rt_mmio_free_token(uint32_t);

static unsigned faults, mmio_reads, mmio_writes, aperture_ops;
static uint64_t last_fault_offset, last_write_offset, last_write_value;
static uint32_t last_write_token;

void dext_pci_transport_record_fault(int kind, uint64_t offset)
{
	assert(kind == DEXT_PCI_FAULT_MMIO);
	++faults;
	last_fault_offset = offset;
}
int dext_pci_removed(void) { return 0; }
int dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *value)
{ (void)token; ++mmio_reads; *value = (uint32_t)offset; return 0; }
int dext_mem_read64(uint32_t token, uint64_t offset, uint64_t *value)
{ (void)token; ++mmio_reads; *value = offset; return 0; }
int dext_mem_write32(uint32_t token, uint64_t offset, uint32_t value)
{ ++mmio_writes; last_write_token = token; last_write_offset = offset; last_write_value = value; return 0; }
int dext_mem_write64(uint32_t token, uint64_t offset, uint64_t value)
{ ++mmio_writes; last_write_token = token; last_write_offset = offset; last_write_value = value; return 0; }
int dext_mem_read8(uint32_t t, uint64_t o, uint8_t *v) { (void)t; (void)o; *v = 0; ++mmio_reads; return 0; }
int dext_mem_read16(uint32_t t, uint64_t o, uint16_t *v) { (void)t; (void)o; *v = 0; ++mmio_reads; return 0; }
int dext_mem_write8(uint32_t t, uint64_t o, uint8_t v) { (void)t; (void)o; (void)v; ++mmio_writes; return 0; }
int dext_mem_write16(uint32_t t, uint64_t o, uint16_t v) { (void)t; (void)o; (void)v; ++mmio_writes; return 0; }
int dext_bar_info(uint8_t bar, uint8_t *index, uint64_t *size) { (void)bar; *index = 0; *size = 0; return -1; }
/* No aperture in these checks. */
int linuxu_aperture_contains(const volatile void *a, size_t s) { (void)a; (void)s; return 0; }
void linuxu_aperture_read(const volatile void *a, void *v, unsigned int w) { (void)a; (void)v; (void)w; ++aperture_ops; }
void linuxu_aperture_write(volatile void *a, const void *v, unsigned int w) { (void)a; (void)v; (void)w; ++aperture_ops; }
void linuxu_aperture_copy_in(volatile void *d, const void *s, size_t n) { (void)d; (void)s; (void)n; ++aperture_ops; }
void linuxu_aperture_copy_out(void *d, const volatile void *s, size_t n) { (void)d; (void)s; (void)n; ++aperture_ops; }
void linuxu_aperture_fill(volatile void *d, int v, size_t n) { (void)d; (void)v; (void)n; ++aperture_ops; }

/* amdgpu_gmc_set_pte_pde's store (amdgpu_gmc.c), into what amdgpu_gart_map
 * is given: here an IB of the copy job, process memory. */
static void set_pte(void *cpu_pt_addr, uint32_t gpu_page_idx, uint64_t addr, uint64_t flags)
{
	uint64_t value = addr & 0x0000FFFFFFFFF000ULL;
	value |= flags;
	writeq(value, ((uint64_t *)cpu_pt_addr) + gpu_page_idx);
}

int main(void)
{
	struct pci_dev dev = {0};

	/* A live register token: MMIO, at its offset. */
	uint32_t registers = rt_mmio_mint_token(&dev, 5, 0, 0x80000, 0);
	assert(registers);
	void *rmmio = (void *)(uintptr_t)rt_mmio_dk_address(registers);
	assert(rt_mmio_dk_is_token((uintptr_t)rmmio) && rt_mmio_dk_token((uintptr_t)rmmio) == registers);
	writel(0x1234, (char *)rmmio + 0x40);
	assert(mmio_writes == 1 && last_write_token == registers && last_write_offset == 0x40 &&
	       last_write_value == 0x1234 && !faults);
	assert(readl((char *)rmmio + 0x80) == 0x80 && mmio_reads == 1);

	/* PTE writes into an IB (heap memory, as 249's 0x127849020 was): plain
	 * stores, no MMIO, no fault. The old decoding (address / stride) made
	 * this token 0, a lost mapping. */
	uint64_t *ib = calloc(64, sizeof(uint64_t));
	assert(ib && !rt_mmio_dk_is_mmio((uintptr_t)ib));
	/* What 249 made of it: a token number (0 for 249's IB; under the
	 * sanitizer's heap, some other), never memory. */
	const uint64_t old_token = (uintptr_t)ib / RT_MMIO_DK_TOKEN_STRIDE;
	assert(old_token < RT_MMIO_DK_MAX_SLOTS);
	for (uint32_t page = 0; page < 8; ++page)
		set_pte(ib + 16, page, 0x123456000ULL + page * 0x1000, 0x7);
	for (uint32_t page = 0; page < 8; ++page)
		assert(ib[16 + page] == ((0x123456000ULL + page * 0x1000) | 0x7));
	assert(readq(ib + 17) == (0x123457000ULL | 0x7));
	memset_io(ib, 0xa5, 8);
	assert(ib[0] == 0xa5a5a5a5a5a5a5a5ULL);
	uint64_t copy[2] = {1, 2};
	memcpy_toio(ib + 2, copy, sizeof(copy));
	memcpy_fromio(copy, ib + 16, sizeof(copy));
	assert(ib[2] == 1 && ib[3] == 2 && copy[0] == (0x123456000ULL | 0x7));
	assert(mmio_writes == 1 && mmio_reads == 1 && !faults && !aperture_ops);

	/* Process memory above 256 GB: under the old scheme this decoded as a
	 * token number (here the live register token's), and a store went to the
	 * registers. Now it is memory. */
	void *hint = (void *)(uintptr_t)(RT_MMIO_DK_TOKEN_STRIDE * (uint64_t)registers + 0x10000000ULL);
	uint64_t *high = mmap(hint, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	assert(high != MAP_FAILED);
	/* Wherever the system put it, it is memory (249 decoded it as token
	 * address / stride: here that is the live register token whenever the
	 * mapping landed in its 256 GB). */
	assert(!rt_mmio_dk_is_mmio((uintptr_t)high));
	writeq(0xfeedULL, high);
	writel(0xbeef, high + 1);
	assert(high[0] == 0xfeed && (uint32_t)high[1] == 0xbeef);
	assert(mmio_writes == 1 && !faults);
	if ((uintptr_t)high / RT_MMIO_DK_TOKEN_STRIDE != registers)
		printf("note: the high mapping is at %p, outside the old token %u range\n", (void *)high, registers);
	munmap(high, 16384);

	/* A stale token (unmapped while the session's token lives, so its
	 * number is not reissued) still fails loudly, and touches nothing. */
	uint32_t doorbells = rt_mmio_mint_token(&dev, 2, 0, 0x1000, 0);
	assert(doorbells && doorbells != registers);
	void *stale = (void *)(uintptr_t)rt_mmio_dk_address(doorbells);
	rt_mmio_free_token(doorbells);
	writel(1, (char *)stale + 8);
	assert(faults == 1 && last_fault_offset == 8 && mmio_writes == 1);
	assert(readl((char *)stale + 12) == UINT32_MAX && faults == 2 && mmio_reads == 1);
	/* So does token 0's range (never minted) and a null-based address. */
	writeq(1, (void *)(uintptr_t)(RT_MMIO_DK_TOKEN_BASE + 0x100));
	assert(faults == 3 && mmio_writes == 1);
	assert(readl((void *)(uintptr_t)0x1000) == UINT32_MAX && faults == 4);

	rt_mmio_free_token(registers);
	free(ib);
	puts("PASS DriverKit accessors: tokens in their own range go to MMIO, a stale one faults, "
	     "IB and other process memory (PTE writes) are plain loads and stores");
	return 0;
}
