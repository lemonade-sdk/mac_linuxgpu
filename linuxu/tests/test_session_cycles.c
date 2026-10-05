/* Session open/probe/close cycles against the real upstream amdgpu probe and
 * the production DriverKit PCI, MMIO-token and heap code (mock IOPCIDevice).
 *
 * Each cycle is what one driver session does: open the PCI session and mint
 * its register token (dext_open), create the runtime device, run upstream's
 * probe (it maps BAR5 and fails later, as an unanswered device does), remove
 * it, free the runtime device and close the PCI session (dext_close).
 * Build 242 lost MMIO token numbers on every cycle and failed the probe
 * with -ENOMEM after about 120 sessions. Every resource a session takes
 * must be back at its baseline after each close. */
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_memory_sysctl.h"

#include <linux/pci.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <rt/bootstrap.h>
#include <rt/dart.h>
#include <rt/dext_dma.h>
#include <rt/dext_pci.h>
#include <rt/rt.h>
#include "dext_heap_backend.h"

/* The cycle has no endpoint capable of a function reset, and the device
 * never leaves the bus. */
int dext_pci_function_reset(void) { return -95; }
int dext_pci_removed(void) { return 0; }

extern uint32_t rt_mmio_mint_token(struct pci_dev *dev, uint8_t mem_index,
				   uint64_t base, uint64_t size, int host_shadow);
extern void rt_mmio_free_token(uint32_t token);

#define BAR0_BYTES (16ULL << 30)
#define BAR5_BYTES (512UL << 10)

static uint16_t pci_command;
static unsigned long mmio_reads, mmio_writes;
static unsigned int bar0_maps;
static unsigned long bar5_mappings;

int dext_pci_snapshot(struct dext_pci_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->vendor = 0x1002;
	snapshot->device = 0x7551;
	snapshot->subsystem_vendor = 0x1da2;
	snapshot->subsystem_device = 0xe499;
	snapshot->revision = 0xc0;
	snapshot->class_code = 0x030000;
	snapshot->bus = 5;
	snapshot->bar[0] = (struct dext_pci_bar) {
		.base = 0x40000000000ULL, .size = BAR0_BYTES, .present = 1,
	};
	snapshot->bar[5] = (struct dext_pci_bar) {
		.base = 0x40410000000ULL, .size = BAR5_BYTES, .present = 1,
	};
	return 0;
}

int dext_pci_config_read32(uint64_t offset, uint32_t *value)
{
	*value = offset == 0 ? 0x75511002 : 0;
	return 0;
}
int dext_pci_config_read16(uint64_t offset, uint16_t *value)
{
	*value = offset == PCI_COMMAND ? pci_command : 0;
	return 0;
}
int dext_pci_config_read8(uint64_t offset, uint8_t *value)
{
	(void)offset;
	*value = 0;
	return 0;
}
/* No MSI-X capability in this configuration: the table is never read. */
int dext_pci_bar_read32(unsigned int bar, uint64_t offset, uint32_t *value)
{
	(void)bar; (void)offset;
	*value = UINT32_MAX;
	return -1;
}
int dext_pci_bar_write32(unsigned int bar, uint64_t offset, uint32_t value)
{
	(void)bar; (void)offset; (void)value;
	return -1;
}
int dext_pci_config_write32(uint64_t offset, uint32_t value)
{
	(void)offset; (void)value;
	return 0;
}
int dext_pci_config_write16(uint64_t offset, uint16_t value)
{
	if (offset == PCI_COMMAND)
		pci_command = value;
	return 0;
}
int dext_pci_config_write8(uint64_t offset, uint8_t value)
{
	(void)offset; (void)value;
	return 0;
}
int dext_pci_irq_status(unsigned int *armed, unsigned int *type)
{
	*armed = 0;
	*type = DEXT_PCI_IRQ_NONE;
	return 0;
}
int dext_bar_info(uint8_t bar, uint8_t *index, uint64_t *size)
{
	if (bar != 0 && bar != 5)
		return -1;
	if (bar == 5)
		bar5_mappings++;	/* each pci_iomap/ioremap of the registers */
	*index = bar;
	*size = bar ? BAR5_BYTES : BAR0_BYTES;
	return 0;
}
void *dext_bar0_cpu_map(uint64_t offset, uint64_t size)
{
	(void)offset; (void)size;
	bar0_maps++;
	return NULL;
}
int dext_bar0_cpu_unmap(const void *address)
{
	(void)address;
	return 0;
}
int dext_bar0_cpu_contains(const void *address, size_t size)
{
	(void)address; (void)size;
	return 0;
}

/* Registers read as zero: the device answers nothing upstream can use. */
#define MMIO_READ(bits)							\
int dext_mem_read##bits(uint32_t token, uint64_t offset, uint##bits##_t *value) \
{									\
	(void)token; (void)offset;					\
	mmio_reads++;							\
	*value = 0;							\
	return 0;							\
}
#define MMIO_WRITE(bits)						\
int dext_mem_write##bits(uint32_t token, uint64_t offset, uint##bits##_t value) \
{									\
	(void)token; (void)offset; (void)value;				\
	mmio_writes++;							\
	return 0;							\
}
MMIO_READ(8) MMIO_READ(16) MMIO_READ(64)

/* Except the firmware's "IFWI init complete" flag (MP0 C2PMSG_33 bit 31),
 * which upstream's discovery otherwise polls for two seconds. */
int dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *value)
{
	(void)token;
	mmio_reads++;
	*value = offset == 0x16061 * 4 ? 0x80000000u : 0;
	return 0;
}
MMIO_WRITE(8) MMIO_WRITE(16) MMIO_WRITE(32) MMIO_WRITE(64)

void dext_pci_transport_record_fault(int fault, uint64_t offset)
{
	fprintf(stderr, "unexpected transport fault %d at 0x%llx\n", fault,
		(unsigned long long)offset);
	abort();
}

/* dext_open's register token: BAR5, BAR-relative, no host shadow, minted
 * against the IOPCIDevice before the runtime device exists. */
static int iopci_device;
static uint32_t session_open(void)
{
	return rt_mmio_mint_token((struct pci_dev *)&iopci_device, 5, 0, BAR5_BYTES, 0);
}

static unsigned int thread_count(void)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	assert(task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS);
	for (mach_msg_type_number_t i = 0; i < count; i++)
		mach_port_deallocate(mach_task_self(), threads[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)threads,
		      count * sizeof(*threads));
	return count;
}

struct footprint {
	size_t heap_allocations, heap_bytes;
	uint64_t dma_bytes;	/* DART-mapped bytes (IOVA space held) */
	int dma_mappings;
	unsigned long pages;	/* page descriptors held */
	unsigned int threads;
};

static struct footprint footprint(void)
{
	return (struct footprint) {
		.heap_allocations = dext_heap_test_live_allocations(),
		.heap_bytes = dext_heap_test_live_bytes(),
		.dma_bytes = linuxu_dart_used(),
		.dma_mappings = linuxu_dart_table_count(),
		.pages = linuxu_page_descriptors(),
		.threads = thread_count(),
	};
}

/* One session. Returns upstream's probe result. */
static int session_cycle(unsigned int cycle)
{
	uint32_t token = session_open();
	if (token == RT_MMIO_TOKEN_INVALID) {
		fprintf(stderr, "cycle %u: the register token could not be minted\n", cycle);
		exit(1);
	}
	struct rt_device *device = rt_device_alloc();
	if (!device) {
		fprintf(stderr, "cycle %u: runtime device allocation failed\n", cycle);
		exit(1);
	}
	struct pci_dev *pdev = rt_device_get_pdev(device);
	const unsigned long mappings_before = bar5_mappings;
	assert(linuxu_driver_bootstrap() == 0);
	const int result = rt_pci_probe_result(pdev);
	/* Upstream mapped the registers (amdgpu_device_init's ioremap) and
	 * unmapped them on its error path. */
	if (bar5_mappings == mappings_before) {
		fprintf(stderr, "cycle %u: probe %d before mapping the registers\n",
			cycle, result);
		exit(1);
	}
	assert(!pdev->dev.driver && !pci_get_drvdata(pdev));
	assert(!pdev->dev.devres && pdev->enable_cnt == 0);
	linuxu_driver_shutdown();
	rt_device_free(device);
	assert(rt_device_active_pdev() == NULL);
	rt_mmio_free_token(token);
	/* Every MMIO mapping of the session is gone, upstream's included. */
	if (rt_mmio_live_tokens()) {
		fprintf(stderr, "cycle %u: %u MMIO mappings outlived the session\n",
			cycle, rt_mmio_live_tokens());
		exit(1);
	}
	return result;
}

int main(int argc, char **argv)
{
	unsigned int cycles = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 300;
	assert(cycles >= 2);

	/* The first session initializes lazily created process-wide state. */
	const int expected = session_cycle(0);
	assert(expected < 0 && expected != -EAGAIN);
	const struct footprint baseline = footprint();
	for (unsigned int cycle = 1; cycle < cycles; cycle++) {
		const int result = session_cycle(cycle);
		const struct footprint now = footprint();
		if (result != expected) {
			fprintf(stderr, "cycle %u: probe returned %d, the first session returned %d\n",
				cycle, result, expected);
			return 1;
		}
		if (now.heap_allocations != baseline.heap_allocations ||
		    now.heap_bytes != baseline.heap_bytes) {
			fprintf(stderr, "cycle %u: heap %zu allocations / %zu bytes, baseline %zu / %zu\n",
				cycle, now.heap_allocations, now.heap_bytes,
				baseline.heap_allocations, baseline.heap_bytes);
			return 1;
		}
		if (now.dma_bytes != baseline.dma_bytes ||
		    now.dma_mappings != baseline.dma_mappings) {
			fprintf(stderr, "cycle %u: %d DMA mappings / %llu bytes, baseline %d / %llu\n",
				cycle, now.dma_mappings, (unsigned long long)now.dma_bytes,
				baseline.dma_mappings, (unsigned long long)baseline.dma_bytes);
			return 1;
		}
		if (now.pages != baseline.pages) {
			fprintf(stderr, "cycle %u: %lu page descriptors held, baseline %lu\n",
				cycle, now.pages, baseline.pages);
			return 1;
		}
		if (now.threads != baseline.threads) {
			fprintf(stderr, "cycle %u: %u threads, baseline %u\n",
				cycle, now.threads, baseline.threads);
			return 1;
		}
	}
	printf("%u session open/probe/close cycles: probe %d each time after mapping the registers, "
	       "heap %zu allocations / %zu bytes, %d DMA mappings / %llu bytes, %lu page descriptors "
	       "and %u threads at baseline\n",
	       cycles, expected, baseline.heap_allocations, baseline.heap_bytes,
	       baseline.dma_mappings, (unsigned long long)baseline.dma_bytes, baseline.pages,
	       baseline.threads);
	return 0;
}
