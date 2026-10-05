/* Session open/probe/close cycles against the real upstream amdgpu probe and
 * the production DriverKit PCI, MMIO-token and heap code (mock IOPCIDevice).
 *
 * Each cycle is what one driver session does: open the PCI session and mint
 * its register token (dext_open), create the runtime device, run upstream's
 * probe (it maps BAR5 and fails later, as an unanswered device does), remove
 * it, free the runtime device and close the PCI session (dext_close).
 * Build 242 lost MMIO token numbers on every cycle and failed the probe
 * with -ENOMEM after about 120 sessions. Every resource a session takes
 * must be back at its baseline after each close.
 *
 *   test_session_cycles [CYCLES]
 *     registers read as zero: the probe stops in IP discovery.
 *   test_session_cycles --deep FIXTURES FIRMWARE VRAM_MB BAR0 BAR2 BAR5 [CYCLES]
 *     the card's own IP discovery binary and VBIOS (FIXTURES/ip_discovery.bin
 *     and vbios.rom, scripts/capture-r9700-fixtures.py) and the firmware in
 *     FIRMWARE: upstream reads the discovery binary as a file
 *     (amdgpu_discovery=2), its ROM through SMUIO's ROM_INDEX/ROM_DATA
 *     registers as on the card ("Fetched VBIOS from ROM"), and maps VRAM
 *     (BAR0) and the doorbells (BAR2), so the probe gets past IP discovery
 *     and the VBIOS into the IPs' initialization. Registers otherwise keep
 *     what was written to them and read zero until then. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
#include "../src/fw/fw_table.h"

/* The cycle has no endpoint capable of a function reset, and the device
 * never leaves the bus. */
int dext_pci_function_reset(void) { return -95; }
int dext_pci_removed(void) { return 0; }

extern uint32_t rt_mmio_mint_token(struct pci_dev *dev, uint8_t mem_index,
				   uint64_t base, uint64_t size, int host_shadow);
extern void rt_mmio_free_token(uint32_t token);
extern int rt_mmio_slot_info(uint32_t token, uint8_t *mem_index, uint64_t *size);

#define BAR0_BYTES (16ULL << 30)
#define BAR5_BYTES (512UL << 10)

static uint16_t pci_command;
static unsigned long mmio_reads, mmio_writes;
static unsigned int bar0_maps;
static unsigned long bar5_mappings;

/* include/discovery.h, soc15_hw_ip.h, smuio_14_0_2_offset.h */
#define SMUIO_HWID		4
#define SMUIO_ROM_INDEX		0x00e4	/* regROM_INDEX, base index 0 */
#define SMUIO_ROM_DATA		0x00e5	/* regROM_DATA, base index 0 */
#define MP0_C2PMSG_33		0x16061	/* "IFWI init complete" in bit 31 */
#define RCC_CONFIG_MEMSIZE	0xde3	/* VRAM in MiB */
extern int amdgpu_discovery;

static struct {
	int on;
	uint8_t *rom;
	size_t rom_bytes;
	uint64_t vram_mb, bar0, bar2, bar5;
	uint64_t rom_index, rom_data;	/* MMIO byte offsets */
	uint64_t rom_at, rom_high;	/* ROM_INDEX's address; the furthest read */
	uint8_t *bar0_cpu;		/* the visible VRAM the probe maps */
} deep;

/* The register file of the deep mode: what was written reads back. */
#define REGS (1u << 16)
static struct { uint64_t offset; uint32_t value; int used; } regs[REGS];
static pthread_mutex_t regs_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t *reg_slot(uint64_t offset, int create)
{
	for (uint32_t i = (uint32_t)(offset >> 2) * 2654435761u % REGS, n = 0; n < REGS;
	     i = (i + 1) % REGS, ++n) {
		if (regs[i].used && regs[i].offset == offset)
			return &regs[i].value;
		if (!regs[i].used) {
			if (!create)
				return NULL;
			regs[i].used = 1;
			regs[i].offset = offset;
			return &regs[i].value;
		}
	}
	fprintf(stderr, "deep mode: the register file is full\n");
	abort();
}

static uint32_t deep_read32(uint64_t offset)
{
	uint32_t value = 0;
	pthread_mutex_lock(&regs_lock);
	if (offset == deep.rom_data) {
		/* SMUIO's ROM window: a dword at ROM_INDEX, which advances. */
		if (deep.rom_at + 4 <= deep.rom_bytes)
			memcpy(&value, deep.rom + deep.rom_at, 4);
		else
			value = UINT32_MAX;
		deep.rom_at += 4;
		if (deep.rom_at > deep.rom_high)
			deep.rom_high = deep.rom_at;
	} else if (offset == MP0_C2PMSG_33 * 4) {
		value = 0x80000000u;
	} else if (offset == RCC_CONFIG_MEMSIZE * 4) {
		value = (uint32_t)deep.vram_mb;
	} else {
		const uint32_t *slot = reg_slot(offset, 0);
		value = slot ? *slot : 0;
	}
	pthread_mutex_unlock(&regs_lock);
	return value;
}

static void deep_write32(uint64_t offset, uint32_t value)
{
	pthread_mutex_lock(&regs_lock);
	if (offset == deep.rom_index)
		deep.rom_at = value;
	*reg_slot(offset, 1) = value;
	pthread_mutex_unlock(&regs_lock);
}

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
		.base = 0x40000000000ULL, .size = deep.on ? deep.bar0 : BAR0_BYTES, .present = 1,
	};
	if (deep.on)
		snapshot->bar[2] = (struct dext_pci_bar) {
			.base = 0x40400000000ULL, .size = deep.bar2, .present = 1,
		};
	snapshot->bar[5] = (struct dext_pci_bar) {
		.base = 0x40410000000ULL, .size = deep.on ? deep.bar5 : BAR5_BYTES, .present = 1,
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
	if (bar != 0 && bar != 5 && !(deep.on && bar == 2))
		return -1;
	if (bar == 5)
		bar5_mappings++;	/* each pci_iomap/ioremap of the registers */
	*index = bar;
	if (deep.on)
		*size = bar == 0 ? deep.bar0 : bar == 2 ? deep.bar2 : deep.bar5;
	else
		*size = bar ? BAR5_BYTES : BAR0_BYTES;
	return 0;
}
static int bar0_cpu_inside(const void *address, size_t size)
{
	const uintptr_t a = (uintptr_t)address, base = (uintptr_t)deep.bar0_cpu;
	return deep.bar0_cpu && a >= base && a - base <= deep.bar0 && size <= deep.bar0 - (a - base);
}
void *dext_bar0_cpu_map(uint64_t offset, uint64_t size)
{
	bar0_maps++;
	if (!deep.on || offset > deep.bar0 || size > deep.bar0 - offset)
		return NULL;
	return deep.bar0_cpu + offset;
}
int dext_bar0_cpu_unmap(const void *address)
{
	return bar0_cpu_inside(address, 0);
}
int dext_bar0_cpu_contains(const void *address, size_t size)
{
	return bar0_cpu_inside(address, size);
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
 * which upstream's discovery otherwise polls for two seconds. The deep
 * mode's registers (BAR5) are its register file; its doorbells (BAR2)
 * read zero. */
int dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *value)
{
	uint8_t bar = 0;
	mmio_reads++;
	if (deep.on && rt_mmio_slot_info(token, &bar, NULL) == 0 && bar == 5) {
		*value = deep_read32(offset);
		return 0;
	}
	*value = offset == MP0_C2PMSG_33 * 4 ? 0x80000000u : 0;
	return 0;
}
int dext_mem_write32(uint32_t token, uint64_t offset, uint32_t value)
{
	uint8_t bar = 0;
	mmio_writes++;
	if (deep.on && rt_mmio_slot_info(token, &bar, NULL) == 0 && bar == 5)
		deep_write32(offset, value);
	return 0;
}
MMIO_WRITE(8) MMIO_WRITE(16) MMIO_WRITE(64)

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
	return rt_mmio_mint_token((struct pci_dev *)&iopci_device, 5, 0,
				  deep.on ? deep.bar5 : BAR5_BYTES, 0);
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
	deep.rom_high = 0;
	assert(linuxu_driver_bootstrap() == 0);
	const int result = rt_pci_probe_result(pdev);
	/* Upstream mapped the registers (amdgpu_device_init's ioremap) and
	 * unmapped them on its error path. */
	if (bar5_mappings == mappings_before) {
		fprintf(stderr, "cycle %u: probe %d before mapping the registers\n",
			cycle, result);
		exit(1);
	}
	/* The deep mode got past IP discovery: upstream found SMUIO in the
	 * card's table and read the whole VBIOS through it. */
	if (deep.on && deep.rom_high < deep.rom_bytes) {
		fprintf(stderr, "cycle %u: probe %d after reading %llu of the VBIOS's %zu bytes\n",
			cycle, result, (unsigned long long)deep.rom_high, deep.rom_bytes);
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

static uint8_t *read_whole(const char *path, size_t *bytes)
{
	FILE *f = fopen(path, "rb");
	struct stat st;
	uint8_t *data;
	if (!f || fstat(fileno(f), &st) || st.st_size <= 0) {
		fprintf(stderr, "deep mode: cannot read %s\n", path);
		exit(1);
	}
	data = malloc((size_t)st.st_size);
	if (!data || fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		fprintf(stderr, "deep mode: cannot read %s\n", path);
		exit(1);
	}
	fclose(f);
	*bytes = (size_t)st.st_size;
	return data;
}

/* SMUIO's ROM registers from the discovery binary (include/discovery.h:
 * version 1 header, IP table v4 with 32-bit base addresses). */
static void deep_rom_registers(const uint8_t *bin, size_t bytes)
{
	uint16_t table, num_dies, die, num_ips;
	if (bytes < 60)
		goto bad;
	memcpy(&table, bin + 12, 2);	/* table_list[IP_DISCOVERY].offset */
	if ((size_t)table + 80 > bytes)
		goto bad;
	memcpy(&num_dies, bin + table + 12, 2);
	for (unsigned int d = 0; d < num_dies && d < 16; d++) {
		memcpy(&die, bin + table + 14 + 4 * d + 2, 2);
		if ((size_t)die + 4 > bytes)
			goto bad;
		memcpy(&num_ips, bin + die + 2, 2);
		size_t at = die + 4;
		for (unsigned int i = 0; i < num_ips; i++) {
			uint16_t hw_id;
			if (at + 8 > bytes)
				goto bad;
			memcpy(&hw_id, bin + at, 2);
			const uint8_t instance = bin[at + 2], bases = bin[at + 3];
			if (hw_id == SMUIO_HWID && instance == 0 && bases) {
				uint32_t base;
				if (bin[at + 4] != 14 || bin[at + 5] != 0 || bin[at + 6] != 2) {
					fprintf(stderr, "deep mode: SMUIO %u.%u.%u: this test knows the ROM "
						"registers of SMUIO 14.0.2 only\n", bin[at + 4], bin[at + 5],
						bin[at + 6]);
					exit(1);
				}
				memcpy(&base, bin + at + 8, 4);
				deep.rom_index = ((uint64_t)base + SMUIO_ROM_INDEX) * 4;
				deep.rom_data = ((uint64_t)base + SMUIO_ROM_DATA) * 4;
				return;
			}
			at += 8 + 4u * bases;
		}
	}
	fprintf(stderr, "deep mode: the discovery binary has no SMUIO\n");
	exit(1);
bad:
	fprintf(stderr, "deep mode: the discovery binary is malformed\n");
	exit(1);
}

static void deep_register_firmware(const char *name, const uint8_t *blob, size_t bytes)
{
	const struct fw_entry entry = { .name = name, .blob = blob, .size = bytes };
	if (fw_table_register(&entry)) {
		fprintf(stderr, "deep mode: cannot register %s\n", name);
		exit(1);
	}
}

static void deep_setup(char **argv)
{
	char path[1024];
	size_t bytes;
	uint8_t *bin;
	DIR *dir;
	struct dirent *e;

	deep.vram_mb = strtoull(argv[3], NULL, 0);
	deep.bar0 = strtoull(argv[4], NULL, 0);
	deep.bar2 = strtoull(argv[5], NULL, 0);
	deep.bar5 = strtoull(argv[6], NULL, 0);
	if (!deep.vram_mb || !deep.bar0 || !deep.bar2 || !deep.bar5 || deep.bar0 > (1ULL << 34)) {
		fprintf(stderr, "deep mode: VRAM size and BAR0/2/5 sizes required\n");
		exit(1);
	}
	snprintf(path, sizeof(path), "%s/ip_discovery.bin", argv[1]);
	bin = read_whole(path, &bytes);
	deep_rom_registers(bin, bytes);
	deep_register_firmware("amdgpu/ip_discovery.bin", bin, bytes);
	free(bin);
	snprintf(path, sizeof(path), "%s/vbios.rom", argv[1]);
	deep.rom = read_whole(path, &deep.rom_bytes);
	/* The rest of the firmware upstream asks for, as on the card. */
	dir = opendir(argv[2]);
	if (!dir) {
		fprintf(stderr, "deep mode: no firmware directory %s\n", argv[2]);
		exit(1);
	}
	while ((e = readdir(dir))) {
		char name[300];
		uint8_t *blob;
		if (!strstr(e->d_name, ".bin"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", argv[2], e->d_name);
		snprintf(name, sizeof(name), "amdgpu/%s", e->d_name);
		blob = read_whole(path, &bytes);
		deep_register_firmware(name, blob, bytes);
		free(blob);
	}
	closedir(dir);
	deep.bar0_cpu = mmap(NULL, deep.bar0, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (deep.bar0_cpu == MAP_FAILED) {
		fprintf(stderr, "deep mode: no memory for the VRAM aperture\n");
		exit(1);
	}
	amdgpu_discovery = 2;	/* the discovery binary as a file */
	deep.on = 1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--deep")) {
		if (argc < 8) {
			fprintf(stderr, "usage: %s --deep FIXTURES FIRMWARE VRAM_MB BAR0 BAR2 BAR5 [CYCLES]\n",
				argv[0]);
			return 2;
		}
		deep_setup(argv + 1);
		argv += 7;	/* --deep and its six arguments */
		argc -= 7;
	}
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
	printf("%u session open/probe/close cycles%s: probe %d each time after mapping the registers, "
	       "heap %zu allocations / %zu bytes, %d DMA mappings / %llu bytes, %lu page descriptors "
	       "and %u threads at baseline\n",
	       cycles, deep.on ? " with an IP discovery table and VBIOS" : "",
	       expected, baseline.heap_allocations, baseline.heap_bytes,
	       baseline.dma_mappings, (unsigned long long)baseline.dma_bytes, baseline.pages,
	       baseline.threads);
	return 0;
}
