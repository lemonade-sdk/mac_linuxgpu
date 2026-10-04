/* A fixture amdgpu device for kernel-queue command submission (cs_fixture.h).
 *
 * The device is brought up the way amdgpu_device_init and
 * amdgpu_device_ip_init bring one up, with the IP-specific parts replaced:
 * upstream TTM (amdgpu_ttm_init: VRAM, GTT, doorbell and on-chip managers),
 * the GART, write-back slots, seq64, the IB pool, amdgpu_vm_manager_init,
 * amdgpu_ring_init with its fence driver, drm_sched schedulers with
 * amdgpu_sched_ops, TTM's buffer-function entities, and a registered DRM
 * device whose render node opens through drm_stub_open, drm_open and
 * amdgpu_driver_open_kms. Above the rings everything is upstream.
 *
 * Below them a software GPU runs: one engine thread per ring reads what
 * the upstream ring code committed (amdgpu_ring_commit -> set_wptr) and
 * executes it, translating GPU addresses as the hardware does: VMID 0
 * through the GART table upstream filled (or the VRAM aperture), other
 * VMIDs by walking the page tables upstream wrote through the SDMA engine.
 * The compute engine executes PM4 (INDIRECT_BUFFER, WRITE_DATA, DMA_DATA,
 * NOP, RELEASE_MEM in IBs and the ring's fence release); with
 * cs_fixture_model_driver_streams() it also follows chained IBs and skips
 * what it does not model, as a full driver's streams need. The SDMA
 * engine executes the fixture's own packet set
 * that its buffer functions and VM PTE functions emit (fill, copy, PTE
 * writes). A fence with an interrupt runs amdgpu_fence_process, as the
 * EOP/trap interrupt handlers do.
 *
 * The fixture's packets are not any real SDMA generation's: the point is
 * that upstream builds, schedules, fences and waits for them. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int usleep(unsigned int usec);

#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/sysinfo.h>
#include <linux/workqueue.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/gpu_scheduler.h>
#include <rt/bootstrap.h>
#include <rt/compute.h>
#include <rt/dart.h>

#include "amdgpu.h"
#include "amdgpu_reset.h"
#include "amdgpu_drv.h"
#include "amdgpu_dma_buf.h"
#include "amdgpu_vm.h"
#include "amdgpu_sdma.h"
#include "amdgpu_ttm.h"
#include "nvd.h"
#include "cs_fixture.h"

extern int linuxu_module_init_drm_core_init(void);
extern int linuxu_module_init_gpu_buddy_module_init(void);
extern int linuxu_module_init_drm_sched_fence_slab_init(void);
extern int linuxu_workqueue_init(void);
extern int linuxu_timer_service_init(void);
extern int amdgpu_sync_init(void);
extern const struct drm_sched_backend_ops amdgpu_sched_ops;
extern int amdgpu_gpu_recovery;

#define FX_ABORT(...) do { fprintf(stderr, "cs fixture: " __VA_ARGS__); \
	fprintf(stderr, "\n"); abort(); } while (0)

#define FX_VRAM_START	0x8000000000ULL
#define FX_VRAM_BYTES	(256ULL << 20)
#define FX_GART_START	0x10000000000ULL
#define FX_GART_BYTES	(512ULL << 20)
#define FX_BAR0		0xe0000000ULL	/* VRAM aperture bus address */
#define FX_BAR2		0xfc000000ULL	/* doorbells */
#define FX_BAR2_BYTES	(2ULL << 20)
#define FX_BAR5		0xfe000000ULL	/* registers */

/* Fixture packet opcodes: PM4 type-3 on compute, (op << 24 | count) on SDMA. */
#define FX_PM4_VM_FLUSH	0x7e
enum {
	FX_SDMA_NOP = 0, FX_SDMA_IB, FX_SDMA_FENCE, FX_SDMA_TRAP, FX_SDMA_WRITE_PTE,
	FX_SDMA_COPY_PTE, FX_SDMA_SET_PTE_PDE, FX_SDMA_COPY, FX_SDMA_FILL, FX_SDMA_VM_FLUSH,
};
#define FX_SDMA(op, count)	(((uint32_t)(op) << 24) | (count))

static struct pci_dev *pdev;
static struct amdgpu_device *adev;
uint64_t cs_fixture_visible_vram;
unsigned int cs_fixture_sdma_instances;
void (*cs_fixture_display)(struct amdgpu_device *adev);
static uint8_t *vram;		/* the VRAM behind the aperture */
static uint64_t *doorbells;
static struct amdgpu_irq_src fence_irq;
static struct cs_fixture_stats stats;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static bool driver_streams;

#define STAT(field) do { pthread_mutex_lock(&stats_lock); stats.field++; \
	pthread_mutex_unlock(&stats_lock); } while (0)

/* ---- address translation ---- */

static uint64_t pte_address(uint64_t entry)
{
	return entry & 0x0000fffffffff000ULL;
}

/* System memory at DMA address @dma, as the IOMMU passes it: a GPU page
 * outside every live DMA mapping is a DART fault (a write to it, behind a
 * Thunderbolt tunnel, can cost the device). Host DMA addresses are host
 * pointers. */
static uint8_t *system_page(uint64_t dma)
{
	if (!linuxu_dart_contains(dma & ~(uint64_t)(AMDGPU_GPU_PAGE_SIZE - 1),
				  AMDGPU_GPU_PAGE_SIZE)) {
		STAT(dart_faults);
		fprintf(stderr, "cs fixture: DART fault, DMA address 0x%llx is not mapped\n",
			(unsigned long long)dma);
		return NULL;
	}
	return (uint8_t *)(uintptr_t)dma;
}

/* Host address of MC address @mc (VMID 0): VRAM or GART. */
static uint8_t *mc_to_host(uint64_t mc)
{
	if (mc >= adev->gmc.vram_start && mc <= adev->gmc.vram_end)
		return vram + (mc - adev->gmc.vram_start);
	if (mc >= adev->gmc.gart_start && mc <= adev->gmc.gart_end) {
		uint64_t index = (mc - adev->gmc.gart_start) >> AMDGPU_GPU_PAGE_SHIFT;
		uint64_t entry = ((uint64_t *)adev->gart.ptr)[index];

		if (!(entry & AMDGPU_PTE_VALID))
			return NULL;
		return system_page(pte_address(entry) + (mc & (AMDGPU_GPU_PAGE_SIZE - 1)));
	}
	return NULL;
}

/* An entry's target: system memory by host address, else an MC address. */
static uint8_t *entry_to_host(uint64_t entry, uint64_t offset)
{
	if (entry & AMDGPU_PTE_SYSTEM)
		return system_page(pte_address(entry) + offset);
	return mc_to_host(pte_address(entry) + offset);
}

static unsigned int level_shift(unsigned int level)
{
	return level == AMDGPU_VM_PTB ? 0 :
		9 * (AMDGPU_VM_PDB0 - level) + adev->vm_manager.block_size;
}

static uint64_t level_entries(unsigned int level)
{
	unsigned int shift = level_shift(adev->vm_manager.root_level);

	if (level == adev->vm_manager.root_level)
		return round_up(adev->vm_manager.max_pfn, 1ULL << shift) >> shift;
	if (level != AMDGPU_VM_PTB)
		return 512;
	return AMDGPU_VM_PTE_COUNT(adev);
}

/* Walk VMID @vmid's page tables (root at MC address @pd) for @va. */
static uint8_t *vm_to_host(uint64_t pd, uint64_t va)
{
	uint64_t pfn = va >> AMDGPU_GPU_PAGE_SHIFT;
	/* amdgpu_gmc_pd_addr: the root's address with PDE flags in the
	 * low bits (VALID, and SYSTEM for a root in GTT). */
	uint64_t table = pte_address(pd);
	bool system = !!(pd & AMDGPU_PTE_SYSTEM);

	for (unsigned int level = adev->vm_manager.root_level;; ++level) {
		unsigned int shift = level_shift(level);
		uint64_t index = (pfn >> shift) & (level_entries(level) - 1);
		uint8_t *at = system ? system_page(table) : mc_to_host(table);
		uint64_t entry;

		if (!at)
			return NULL;
		memcpy(&entry, at + index * 8, 8);
		if (!(entry & AMDGPU_PTE_VALID)) {
			if (getenv("CS_FIXTURE_TRACE"))
				fprintf(stderr, "walk 0x%llx: level %u index %llu entry 0x%llx\n",
					(unsigned long long)va, level, (unsigned long long)index,
					(unsigned long long)entry);
			return NULL;
		}
		if (level == AMDGPU_VM_PTB || (entry & AMDGPU_PDE_PTE_FLAG(adev))) {
			uint64_t span = (pfn & ((1ULL << shift) - 1)) << AMDGPU_GPU_PAGE_SHIFT;

			return entry_to_host(entry, span + (va & (AMDGPU_GPU_PAGE_SIZE - 1)));
		}
		system = !!(entry & AMDGPU_PTE_SYSTEM);
		table = pte_address(entry);
		if (level == AMDGPU_VM_PTB)
			return NULL;
	}
}

struct engine;
static uint8_t *gpu_to_host(struct engine *e, uint32_t vmid, uint64_t addr);

/* Copy between GPU memory and the host, a GPU page at a time. */
static bool gpu_access(struct engine *e, uint32_t vmid, uint64_t addr, void *buf,
		       uint64_t bytes, bool write)
{
	while (bytes) {
		uint64_t room = AMDGPU_GPU_PAGE_SIZE - (addr & (AMDGPU_GPU_PAGE_SIZE - 1));
		uint64_t n = bytes < room ? bytes : room;
		uint8_t *at = gpu_to_host(e, vmid, addr);

		if (!at) {
			STAT(faults);
			fprintf(stderr, "cs fixture: GPU fault, vmid %u address 0x%llx\n", vmid,
				(unsigned long long)addr);
			return false;
		}
		if (write)
			memcpy(at, buf, n);
		else
			memcpy(buf, at, n);
		addr += n;
		buf = (uint8_t *)buf + n;
		bytes -= n;
	}
	return true;
}

/* ---- engines ---- */

struct engine {
	struct amdgpu_ring *ring;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t kick;
	uint64_t wptr;		/* what set_wptr published */
	uint64_t rptr;
	bool stop, hold;
};
/* The VMID page-table registers, per hub as in the hardware: an engine's
 * VM flush programs the hub every engine on it translates through. */
static uint64_t hub_pd[AMDGPU_MAX_VMHUBS][AMDGPU_NUM_VMID];
static pthread_mutex_t hub_lock = PTHREAD_MUTEX_INITIALIZER;

static void hub_set_pd(struct engine *e, uint32_t vmid, uint64_t pd)
{
	if (vmid >= AMDGPU_NUM_VMID)
		FX_ABORT("VM flush of vmid %u", vmid);
	pthread_mutex_lock(&hub_lock);
	hub_pd[e->ring->vm_hub][vmid] = pd;
	pthread_mutex_unlock(&hub_lock);
}
static struct engine compute_engine, sdma_engine, sdma1_engine;

static uint8_t *gpu_to_host(struct engine *e, uint32_t vmid, uint64_t addr)
{
	uint64_t pd;

	if (!vmid)
		return mc_to_host(addr);
	if (vmid >= AMDGPU_NUM_VMID)
		return NULL;
	pthread_mutex_lock(&hub_lock);
	pd = hub_pd[e->ring->vm_hub][vmid];
	pthread_mutex_unlock(&hub_lock);
	return pd ? vm_to_host(pd, addr) : NULL;
}

static struct engine *engine_of(struct amdgpu_ring *ring)
{
	if (ring == &adev->gfx.compute_ring[0])
		return &compute_engine;
	return ring == &adev->sdma.instance[1].ring ? &sdma1_engine : &sdma_engine;
}

static uint64_t fx_get_rptr(struct amdgpu_ring *ring)
{
	return *ring->rptr_cpu_addr;
}

static uint64_t fx_get_wptr(struct amdgpu_ring *ring)
{
	return ring->wptr;
}

static void fx_set_wptr(struct amdgpu_ring *ring)
{
	struct engine *e = engine_of(ring);

	pthread_mutex_lock(&e->lock);
	e->wptr = ring->wptr;
	pthread_cond_signal(&e->kick);
	pthread_mutex_unlock(&e->lock);
}

static uint32_t ring_dword(struct amdgpu_ring *ring, uint64_t ptr)
{
	return ring->ring[ptr & ring->buf_mask];
}

/* The fence interrupt: what the EOP and trap handlers do. */
static void interrupt(struct amdgpu_ring *ring)
{
	STAT(interrupts);
	amdgpu_fence_process(ring);
}

static void fence_write(struct engine *e, uint64_t addr, uint64_t seq, bool wide)
{
	STAT(fences);
	/* Fence addresses are kernel (GART/VRAM) addresses on every VMID. */
	if (!gpu_access(e, 0, addr, &seq, wide ? 8 : 4, true))
		FX_ABORT("fence write to unmapped 0x%llx", (unsigned long long)addr);
}

/* -- compute: PM4 -- */

static void pm4_run(struct engine *e, const uint32_t *dw, uint64_t count, uint32_t vmid, int depth);

/* PM4 opcodes of a driver's streams the fixture recognizes but does not
 * model as such (sid.h numbering). */
#define FX_PM4_DISPATCH_DIRECT			0x15
#define FX_PM4_DISPATCH_INDIRECT		0x16
#define FX_PM4_DMA_DATA				0x50
#define FX_PM4_DISPATCH_DIRECT_INTERLEAVED	0xa7

/* CP DMA (DMA_DATA): fill with immediate data or copy, a page at a time. */
static void pm4_dma_data(struct engine *e, const uint32_t *dw, uint32_t vmid)
{
	const uint32_t src_sel = (dw[1] >> 29) & 3, dst_sel = (dw[1] >> 20) & 3;
	const uint64_t src = ((uint64_t)dw[3] << 32) | dw[2];
	const uint64_t dst = ((uint64_t)dw[5] << 32) | dw[4];
	const uint32_t bytes = dw[6] & 0x3ffffff;
	uint8_t chunk[4096];

	STAT(dma_data);
	if (dst_sel == 2)	/* DST_NOWHERE: a prefetch */
		return;
	for (uint32_t off = 0; off < bytes;) {
		uint32_t n = bytes - off < sizeof(chunk) ? bytes - off : sizeof(chunk);

		if (src_sel == 2) {	/* DATA: the immediate dword */
			for (uint32_t i = 0; i < n; i += 4)
				memcpy(chunk + i, &dw[2], 4);
		} else if (!gpu_access(e, vmid, src + off, chunk, n, false)) {
			return;
		}
		if (!gpu_access(e, vmid, dst + off, chunk, n, true))
			return;
		off += n;
	}
}

static uint64_t pm4_packet(struct engine *e, const uint32_t *dw, uint64_t avail, uint32_t vmid,
			   int depth)
{
	uint32_t header = dw[0];
	uint32_t op = (header >> 8) & 0xff, n = ((header >> 16) & 0x3fff) + 1;

	if (header == e->ring->funcs->nop)
		return 1;
	if ((header >> 30) != PACKET_TYPE3 || n + 1 > avail)
		FX_ABORT("bad PM4 header 0x%08x (vmid %u, %s)", header, vmid,
			 depth ? "IB" : "ring");
	switch (op) {
	case PACKET3_NOP:
		break;
	case PACKET3_INDIRECT_BUFFER: {
		uint64_t va = ((uint64_t)(dw[2] & 0xffff) << 32) | (dw[1] & ~3u);
		uint32_t len = dw[3] & 0xfffff, ib_vmid = depth ? vmid : dw[3] >> 24;
		uint32_t *ib = calloc(len ? len : 1, 4);

		/* Inside an IB: a chained (or nested) IB of the same VMID. */
		if (depth && (!driver_streams || depth > 4))
			FX_ABORT("chained IB");
		STAT(compute_ibs);
		if (!ib || !gpu_access(e, ib_vmid, va, ib, (uint64_t)len * 4, false))
			FX_ABORT("compute IB at 0x%llx (vmid %u) does not translate",
				 (unsigned long long)va, ib_vmid);
		pm4_run(e, ib, len, ib_vmid, depth + 1);
		free(ib);
		break;
	}
	case PACKET3_WRITE_DATA: {
		uint64_t va = ((uint64_t)dw[3] << 32) | dw[2];

		if (((dw[1] >> 8) & 0xf) != 5) {
			if (!driver_streams)
				FX_ABORT("WRITE_DATA to a non-memory destination");
			STAT(skipped);	/* a register write */
			break;
		}
		STAT(write_data);
		gpu_access(e, vmid, va, (void *)(uintptr_t)&dw[4], (uint64_t)(n - 3) * 4, true);
		break;
	}
	case FX_PM4_DMA_DATA:
		if (n != 6)
			FX_ABORT("DMA_DATA of %u dwords", n);
		pm4_dma_data(e, dw, vmid);
		break;
	case PACKET3_RELEASE_MEM: {
		uint64_t addr, seq;

		if (n == 7) {
			/* A driver's end-of-pipe release (GFX9+ layout):
			 * event, DST/INT/DATA_SEL, address, data. Work is
			 * done in order here, so the event has happened. */
			uint32_t data_sel = dw[2] >> 29;

			addr = ((uint64_t)dw[4] << 32) | dw[3];
			seq = ((uint64_t)dw[6] << 32) | dw[5];
			STAT(release_mem);
			if (data_sel == 3)	/* the GPU clock */
				seq = (uint64_t)ktime_get_ns() / 10;
			if (data_sel >= 1 && data_sel <= 3)
				gpu_access(e, vmid, addr, &seq, data_sel == 1 ? 4 : 8, true);
			break;
		}
		/* The fixture's fence: flags, address, sequence. */
		addr = ((uint64_t)dw[3] << 32) | dw[2];
		seq = ((uint64_t)dw[5] << 32) | dw[4];
		fence_write(e, addr, seq, dw[1] & AMDGPU_FENCE_FLAG_64BIT);
		if (dw[1] & AMDGPU_FENCE_FLAG_INT)
			interrupt(e->ring);
		break;
	}
	case FX_PM4_VM_FLUSH:
		if (depth)
			goto unmodeled;	/* opcode 0x7e in an IB is not ours */
		STAT(vm_flushes);
		hub_set_pd(e, dw[1], ((uint64_t)dw[3] << 32) | dw[2]);
		break;
	case FX_PM4_DISPATCH_DIRECT:
	case FX_PM4_DISPATCH_INDIRECT:
	case FX_PM4_DISPATCH_DIRECT_INTERLEAVED:
		if (!driver_streams || !depth)
			FX_ABORT("dispatch packet 0x%02x", op);
		STAT(dispatches);
		break;
	default:
	unmodeled:
		if (!driver_streams || !depth)
			FX_ABORT("unexpected PM4 opcode 0x%02x", op);
		if (getenv("CS_FIXTURE_TRACE"))
			fprintf(stderr, "skip PM4 opcode 0x%02x (%u dwords)\n", op, n + 1);
		STAT(skipped);
		break;
	}
	return n + 1;
}

static void pm4_run(struct engine *e, const uint32_t *dw, uint64_t count, uint32_t vmid, int depth)
{
	for (uint64_t i = 0; i < count;)
		i += pm4_packet(e, dw + i, count - i, vmid, depth);
}

/* -- SDMA: the fixture's packets -- */

static void sdma_run(struct engine *e, const uint32_t *dw, uint64_t count, uint32_t vmid, int depth);

static uint64_t sdma_packet(struct engine *e, const uint32_t *dw, uint64_t avail, uint32_t vmid,
			    int depth)
{
	uint32_t op = dw[0] >> 24, n = dw[0] & 0xffffff;

	if (n + 1 > avail)
		FX_ABORT("SDMA packet 0x%08x past the end", dw[0]);
	switch (op) {
	case FX_SDMA_NOP:
		break;
	case FX_SDMA_IB: {
		uint64_t va = ((uint64_t)dw[2] << 32) | dw[1];
		uint32_t len = dw[3], ib_vmid = dw[4];
		uint32_t *ib = calloc(len ? len : 1, 4);

		if (depth)
			FX_ABORT("chained SDMA IB");
		STAT(sdma_ibs);
		if (!ib || !gpu_access(e, ib_vmid, va, ib, (uint64_t)len * 4, false))
			FX_ABORT("SDMA IB at 0x%llx (vmid %u) does not translate",
				 (unsigned long long)va, ib_vmid);
		sdma_run(e, ib, len, ib_vmid, depth + 1);
		free(ib);
		break;
	}
	case FX_SDMA_FENCE:
		fence_write(e, ((uint64_t)dw[2] << 32) | dw[1], dw[3], false);
		break;
	case FX_SDMA_TRAP:
		interrupt(e->ring);
		break;
	case FX_SDMA_WRITE_PTE: {
		uint64_t pe = ((uint64_t)dw[2] << 32) | dw[1];
		uint64_t value = ((uint64_t)dw[5] << 32) | dw[4];

		for (uint32_t i = 0; i < dw[3]; ++i, pe += 8, value += dw[6]) {
			STAT(pte_writes);
			gpu_access(e, vmid, pe, &value, 8, true);
		}
		break;
	}
	case FX_SDMA_COPY_PTE: {
		uint64_t pe = ((uint64_t)dw[2] << 32) | dw[1];
		uint64_t src = ((uint64_t)dw[4] << 32) | dw[3];

		for (uint32_t i = 0; i < dw[5]; ++i, pe += 8, src += 8) {
			uint64_t entry;

			STAT(pte_writes);
			if (gpu_access(e, vmid, src, &entry, 8, false))
				gpu_access(e, vmid, pe, &entry, 8, true);
		}
		break;
	}
	case FX_SDMA_SET_PTE_PDE: {
		uint64_t pe = ((uint64_t)dw[2] << 32) | dw[1];
		uint64_t addr = ((uint64_t)dw[4] << 32) | dw[3];
		uint64_t flags = ((uint64_t)dw[6] << 32) | dw[5];

		for (uint32_t i = 0; i < dw[8]; ++i, pe += 8, addr += dw[7]) {
			uint64_t entry = addr | flags;

			STAT(pte_writes);
			gpu_access(e, vmid, pe, &entry, 8, true);
		}
		break;
	}
	case FX_SDMA_COPY: {
		uint64_t src = ((uint64_t)dw[2] << 32) | dw[1];
		uint64_t dst = ((uint64_t)dw[4] << 32) | dw[3];
		uint8_t *tmp = malloc(dw[5] ? dw[5] : 1);

		STAT(copies);
		if (tmp && gpu_access(e, vmid, src, tmp, dw[5], false))
			gpu_access(e, vmid, dst, tmp, dw[5], true);
		free(tmp);
		break;
	}
	case FX_SDMA_FILL: {
		/* As SDMA 5.2-7's CONSTANT_FILL with FILLSIZE 0 (what their
		 * emit_fill_buffer emits): the value's low byte, per byte. */
		uint64_t dst = ((uint64_t)dw[2] << 32) | dw[1];
		uint8_t pattern[256];

		STAT(fills);
		memset(pattern, dw[3] & 0xff, sizeof(pattern));
		for (uint32_t off = 0; off < dw[4]; off += sizeof(pattern)) {
			uint32_t n = dw[4] - off < sizeof(pattern) ? dw[4] - off : sizeof(pattern);

			gpu_access(e, vmid, dst + off, pattern, n, true);
		}
		break;
	}
	case FX_SDMA_VM_FLUSH:
		STAT(vm_flushes);
		hub_set_pd(e, dw[1], ((uint64_t)dw[3] << 32) | dw[2]);
		break;
	default:
		FX_ABORT("unexpected SDMA opcode %u", op);
	}
	return n + 1;
}

static void sdma_run(struct engine *e, const uint32_t *dw, uint64_t count, uint32_t vmid, int depth)
{
	for (uint64_t i = 0; i < count;)
		i += sdma_packet(e, dw + i, count - i, vmid, depth);
}

/* Execute the ring from rptr to wptr, a packet at a time. */
static void *engine_main(void *arg)
{
	struct engine *e = arg;
	struct amdgpu_ring *ring = e->ring;
	const bool pm4 = ring->funcs->type == AMDGPU_RING_TYPE_COMPUTE;

	for (;;) {
		uint64_t wptr;

		pthread_mutex_lock(&e->lock);
		while (!e->stop && (e->hold || e->rptr == e->wptr))
			pthread_cond_wait(&e->kick, &e->lock);
		if (e->stop) {
			pthread_mutex_unlock(&e->lock);
			return NULL;
		}
		wptr = e->wptr;
		pthread_mutex_unlock(&e->lock);
		while (e->rptr != wptr) {
			uint32_t dw[32];
			uint64_t avail = wptr - e->rptr, take = avail < 32 ? avail : 32, used;

			for (uint64_t i = 0; i < take; ++i)
				dw[i] = ring_dword(ring, e->rptr + i);
			if (getenv("CS_FIXTURE_TRACE"))
				fprintf(stderr, "%s rptr %llu wptr %llu: %08x %08x %08x %08x\n", ring->name,
					(unsigned long long)e->rptr, (unsigned long long)wptr,
					dw[0], take > 1 ? dw[1] : 0, take > 2 ? dw[2] : 0, take > 3 ? dw[3] : 0);
			used = pm4 ? pm4_packet(e, dw, take, 0, 0) : sdma_packet(e, dw, take, 0, 0);
			e->rptr += used;
			*ring->rptr_cpu_addr = e->rptr;
		}
	}
}

/* ---- ring functions ---- */

static void fx_pm4_emit_ib(struct amdgpu_ring *ring, struct amdgpu_job *job,
			   struct amdgpu_ib *ib, uint32_t flags)
{
	(void)flags;
	amdgpu_ring_write(ring, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr) & ~3u);
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr) & 0xffff);
	amdgpu_ring_write(ring, ib->length_dw | (AMDGPU_JOB_GET_VMID(job) << 24));
}

static void fx_pm4_emit_fence(struct amdgpu_ring *ring, uint64_t addr, uint64_t seq,
			      unsigned int flags)
{
	amdgpu_ring_write(ring, PACKET3(PACKET3_RELEASE_MEM, 4));
	amdgpu_ring_write(ring, flags);
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));
	amdgpu_ring_write(ring, upper_32_bits(seq));
}

static void fx_pm4_emit_vm_flush(struct amdgpu_ring *ring, unsigned int vmid, uint64_t pd_addr)
{
	amdgpu_ring_write(ring, PACKET3(FX_PM4_VM_FLUSH, 2));
	amdgpu_ring_write(ring, vmid);
	amdgpu_ring_write(ring, lower_32_bits(pd_addr));
	amdgpu_ring_write(ring, upper_32_bits(pd_addr));
}

static void fx_pm4_emit_hdp_flush(struct amdgpu_ring *ring)
{
	amdgpu_ring_write(ring, PACKET3(PACKET3_NOP, 0));
	amdgpu_ring_write(ring, 0);
}

static const struct amdgpu_ring_funcs fx_compute_funcs = {
	.type = AMDGPU_RING_TYPE_COMPUTE,
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = true,
	.get_rptr = fx_get_rptr,
	.get_wptr = fx_get_wptr,
	.set_wptr = fx_set_wptr,
	.emit_frame_size = 4 + 6 * 3 + 2,
	.emit_ib_size = 4,
	.emit_ib = fx_pm4_emit_ib,
	.emit_fence = fx_pm4_emit_fence,
	.emit_vm_flush = fx_pm4_emit_vm_flush,
	.emit_hdp_flush = fx_pm4_emit_hdp_flush,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
};

static void fx_sdma_emit_ib(struct amdgpu_ring *ring, struct amdgpu_job *job,
			    struct amdgpu_ib *ib, uint32_t flags)
{
	(void)flags;
	amdgpu_ring_write(ring, FX_SDMA(FX_SDMA_IB, 4));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, ib->length_dw);
	amdgpu_ring_write(ring, AMDGPU_JOB_GET_VMID(job));
}

static void fx_sdma_emit_fence(struct amdgpu_ring *ring, uint64_t addr, uint64_t seq,
			       unsigned int flags)
{
	/* As SDMA does: a 32-bit write per half, then the trap. */
	amdgpu_ring_write(ring, FX_SDMA(FX_SDMA_FENCE, 3));
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));
	if (flags & AMDGPU_FENCE_FLAG_64BIT) {
		addr += 4;
		amdgpu_ring_write(ring, FX_SDMA(FX_SDMA_FENCE, 3));
		amdgpu_ring_write(ring, lower_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(seq));
	}
	if (flags & AMDGPU_FENCE_FLAG_INT)
		amdgpu_ring_write(ring, FX_SDMA(FX_SDMA_TRAP, 0));
}

static void fx_sdma_emit_vm_flush(struct amdgpu_ring *ring, unsigned int vmid, uint64_t pd_addr)
{
	amdgpu_ring_write(ring, FX_SDMA(FX_SDMA_VM_FLUSH, 3));
	amdgpu_ring_write(ring, vmid);
	amdgpu_ring_write(ring, lower_32_bits(pd_addr));
	amdgpu_ring_write(ring, upper_32_bits(pd_addr));
}

static void fx_sdma_pad_ib(struct amdgpu_ring *ring, struct amdgpu_ib *ib)
{
	(void)ring;
	while (ib->length_dw & 7)
		ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_NOP, 0);
}

static const struct amdgpu_ring_funcs fx_sdma_funcs = {
	.type = AMDGPU_RING_TYPE_SDMA,
	.align_mask = 0xf,
	.nop = FX_SDMA(FX_SDMA_NOP, 0),
	.support_64bit_ptrs = true,
	.get_rptr = fx_get_rptr,
	.get_wptr = fx_get_wptr,
	.set_wptr = fx_set_wptr,
	.emit_frame_size = 4 + 9 + 4,
	.emit_ib_size = 5,
	.emit_ib = fx_sdma_emit_ib,
	.emit_fence = fx_sdma_emit_fence,
	.emit_vm_flush = fx_sdma_emit_vm_flush,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = fx_sdma_pad_ib,
};

/* ---- buffer and PTE functions (TTM moves/clears, VM updates) ---- */

static void fx_emit_copy_buffer(struct amdgpu_ib *ib, uint64_t src, uint64_t dst,
				uint32_t bytes, uint32_t flags)
{
	(void)flags;
	ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_COPY, 5);
	ib->ptr[ib->length_dw++] = lower_32_bits(src);
	ib->ptr[ib->length_dw++] = upper_32_bits(src);
	ib->ptr[ib->length_dw++] = lower_32_bits(dst);
	ib->ptr[ib->length_dw++] = upper_32_bits(dst);
	ib->ptr[ib->length_dw++] = bytes;
}

static void fx_emit_fill_buffer(struct amdgpu_ib *ib, uint32_t value, uint64_t dst, uint32_t bytes)
{
	ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_FILL, 4);
	ib->ptr[ib->length_dw++] = lower_32_bits(dst);
	ib->ptr[ib->length_dw++] = upper_32_bits(dst);
	ib->ptr[ib->length_dw++] = value;
	ib->ptr[ib->length_dw++] = bytes;
}

static const struct amdgpu_buffer_funcs fx_buffer_funcs = {
	.copy_max_bytes = 0x400000,
	.copy_num_dw = 6,
	.emit_copy_buffer = fx_emit_copy_buffer,
	.fill_max_bytes = 0x400000,
	.fill_num_dw = 5,
	.emit_fill_buffer = fx_emit_fill_buffer,
};

static void fx_copy_pte(struct amdgpu_ib *ib, uint64_t pe, uint64_t src, unsigned int count)
{
	ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_COPY_PTE, 5);
	ib->ptr[ib->length_dw++] = lower_32_bits(pe);
	ib->ptr[ib->length_dw++] = upper_32_bits(pe);
	ib->ptr[ib->length_dw++] = lower_32_bits(src);
	ib->ptr[ib->length_dw++] = upper_32_bits(src);
	ib->ptr[ib->length_dw++] = count;
}

static void fx_write_pte(struct amdgpu_ib *ib, uint64_t pe, uint64_t value, unsigned int count,
			 uint32_t incr)
{
	ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_WRITE_PTE, 6);
	ib->ptr[ib->length_dw++] = lower_32_bits(pe);
	ib->ptr[ib->length_dw++] = upper_32_bits(pe);
	ib->ptr[ib->length_dw++] = count;
	ib->ptr[ib->length_dw++] = lower_32_bits(value);
	ib->ptr[ib->length_dw++] = upper_32_bits(value);
	ib->ptr[ib->length_dw++] = incr;
}

static void fx_set_pte_pde(struct amdgpu_ib *ib, uint64_t pe, uint64_t addr, unsigned int count,
			   uint32_t incr, uint64_t flags)
{
	ib->ptr[ib->length_dw++] = FX_SDMA(FX_SDMA_SET_PTE_PDE, 8);
	ib->ptr[ib->length_dw++] = lower_32_bits(pe);
	ib->ptr[ib->length_dw++] = upper_32_bits(pe);
	ib->ptr[ib->length_dw++] = lower_32_bits(addr);
	ib->ptr[ib->length_dw++] = upper_32_bits(addr);
	ib->ptr[ib->length_dw++] = lower_32_bits(flags);
	ib->ptr[ib->length_dw++] = upper_32_bits(flags);
	ib->ptr[ib->length_dw++] = incr;
	ib->ptr[ib->length_dw++] = count;
}

static const struct amdgpu_vm_pte_funcs fx_pte_funcs = {
	.copy_pte_num_dw = 6,
	.copy_pte = fx_copy_pte,
	.write_pte = fx_write_pte,
	.set_pte_pde = fx_set_pte_pde,
};

/* ---- GMC, HDP, IRQ, ASIC ---- */

static void fx_flush_gpu_tlb(struct amdgpu_device *a, uint32_t vmid, uint32_t vmhub,
			     uint32_t flush_type)
{
	(void)a; (void)vmid; (void)vmhub; (void)flush_type;
}

static void fx_flush_gpu_tlb_pasid(struct amdgpu_device *a, uint16_t pasid, uint32_t flush_type,
				   bool all_hub, uint32_t inst)
{
	(void)a; (void)pasid; (void)flush_type; (void)all_hub; (void)inst;
}

static void fx_get_vm_pde(struct amdgpu_device *a, int level, uint64_t *addr, uint64_t *flags)
{
	/* PDEs keep MC addresses: the walker reads them as the GPU would
	 * after the FB-offset translation real GMCs apply here. */
	(void)a; (void)level; (void)addr; (void)flags;
}

static void fx_get_vm_pte(struct amdgpu_device *a, struct amdgpu_vm *vm, struct amdgpu_bo *bo,
			  uint32_t vm_flags, uint64_t *flags)
{
	(void)a; (void)vm; (void)bo; (void)vm_flags; (void)flags;
}

static const struct amdgpu_gmc_funcs fx_gmc_funcs = {
	.flush_gpu_tlb = fx_flush_gpu_tlb,
	.flush_gpu_tlb_pasid = fx_flush_gpu_tlb_pasid,
	.get_vm_pde = fx_get_vm_pde,
	.get_vm_pte = fx_get_vm_pte,
};

static void fx_hdp(struct amdgpu_device *a, struct amdgpu_ring *ring)
{
	(void)a; (void)ring;
}

static const struct amdgpu_hdp_funcs fx_hdp_funcs = {
	.flush_hdp = fx_hdp,
	.invalidate_hdp = fx_hdp,
};

static int fx_irq_set(struct amdgpu_device *a, struct amdgpu_irq_src *src, unsigned int type,
		      enum amdgpu_interrupt_state state)
{
	(void)a; (void)src; (void)type; (void)state;
	return 0;
}

static const struct amdgpu_irq_src_funcs fx_irq_funcs = {
	.set = fx_irq_set,
};

static uint32_t fx_get_xclk(struct amdgpu_device *a)
{
	(void)a;
	return 10000;	/* 100 MHz in 10 kHz units */
}

/* The R9700's (Navi 48, gfx1201) GB_ADDR_CONFIG, as Mesa's gfx1201
 * profile (src/amd/common/amdgpu_devices.c) records it. */
#define FX_MM_GB_ADDR_CONFIG	0x263e
#define FX_GB_ADDR_CONFIG	0x08200545

/* AMDGPU_INFO_READ_MMR_REG: the registers userspace drivers ask for. */
static int fx_read_register(struct amdgpu_device *a, u32 se, u32 sh, u32 reg, u32 *value)
{
	(void)a; (void)se; (void)sh;
	if (reg != FX_MM_GB_ADDR_CONFIG)
		return -EINVAL;
	*value = FX_GB_ADDR_CONFIG;
	return 0;
}

static const struct amdgpu_asic_funcs fx_asic_funcs = {
	.read_register = fx_read_register,
	.get_xclk = fx_get_xclk,
};

/* The shader array layout, caches and firmware versions gfx_v12_0 reads
 * from an R9700 (Mesa's gfx1201 profile): what AMDGPU_INFO_DEV_INFO and
 * AMDGPU_INFO_FW_VERSION report, so a userspace driver sizes itself as it
 * would on the GPU. */
static void fx_gfx_config(void)
{
	struct amdgpu_gfx_config *c = &adev->gfx.config;
	struct amdgpu_cu_info *cu = &adev->gfx.cu_info;

	c->max_shader_engines = 4;
	c->max_sh_per_se = 2;
	c->max_cu_per_sh = 8;
	c->max_backends_per_se = 4;
	c->backend_enable_mask = 0xffff;
	c->max_hw_contexts = 8;
	c->max_gprs = 1536;
	c->max_texture_channel_caches = 32;
	c->gs_vgt_table_depth = 32;
	c->gs_prim_buffer_depth = 1792;
	c->max_gs_threads = 32;
	c->double_offchip_lds_buf = 64;
	c->gb_addr_config = FX_GB_ADDR_CONFIG;
	c->gc_tcp_l1_size = 32;
	c->gc_num_sqc_per_wgp = 1;
	c->gc_l1_data_cache_size_per_sqc = 16;
	c->gc_l1_instruction_cache_size_per_sqc = 32;
	c->gc_gl1c_size_per_instance = 256;
	c->gc_gl1c_per_sa = 1;
	c->gc_gl2c_per_gpu = 8192;
	adev->gmc.mall_size = 64ULL << 20;
	cu->number = 64;
	cu->wave_front_size = 32;
	cu->simd_per_cu = 2;
	cu->max_waves_per_simd = 16;
	cu->lds_size = 128;
	for (int se = 0; se < 4; ++se)
		for (int sh = 0; sh < 2; ++sh)
			cu->bitmap[0][se][sh] = 0xff;
	adev->clock.default_sclk = 246000;	/* 10 kHz units */
	adev->clock.default_mclk = 125800;
	adev->rev_id = 0x01;
	adev->external_rev_id = 0x51;
	adev->gfx.me_fw_version = 2590;
	adev->gfx.me_feature_version = 29;
	adev->gfx.pfp_fw_version = 2630;
	adev->gfx.pfp_feature_version = 29;
	adev->gfx.mec_fw_version = 2800;
	adev->gfx.mec_feature_version = 29;
}

static const struct amdgpu_gfx_funcs fx_gfx_funcs;
static const struct amdgpu_rlc_funcs fx_rlc_funcs;

static const struct amd_ip_funcs fx_ip_funcs = { .name = "cs_fixture" };
static const struct amdgpu_ip_block_version fx_gfx_block = {
	.type = AMD_IP_BLOCK_TYPE_GFX, .major = 12, .minor = 0, .funcs = &fx_ip_funcs,
};
static const struct amdgpu_ip_block_version fx_sdma_block = {
	.type = AMD_IP_BLOCK_TYPE_SDMA, .major = 7, .minor = 0, .funcs = &fx_ip_funcs,
};

/* ---- the DRM driver: amdgpu_kms_driver's operations ---- */

static const struct file_operations fx_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = amdgpu_drm_ioctl,
	.mmap = drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

static struct drm_driver fx_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	.open = amdgpu_driver_open_kms,
	.postclose = amdgpu_driver_postclose_kms,
	.ioctls = amdgpu_ioctls_kms,
	/* ARRAY_SIZE(amdgpu_ioctls_kms): indexed by command, the last is
	 * GEM_LIST_HANDLES. */
	.num_ioctls = DRM_AMDGPU_GEM_LIST_HANDLES + 1,
	.fops = &fx_fops,
	.gem_prime_import = amdgpu_gem_prime_import,
	.name = "amdgpu",
	.desc = "AMD GPU (cs fixture)",
	.major = 3,
	.minor = 64,
};

/* ---- bring-up ---- */

static void fx_noop_work(struct work_struct *work)
{
	(void)work;
}

static void ring_setup(struct amdgpu_ring *ring, const struct amdgpu_ring_funcs *funcs,
		       const char *name, unsigned int vm_hub, struct engine *e)
{
	struct drm_sched_init_args args = {
		.ops = &amdgpu_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = 0,
		.timeout = msecs_to_jiffies(10000),
		.timeout_wq = adev->reset_domain->wq,
		.name = name,
		.dev = adev->dev,
	};
	int r;

	ring->funcs = funcs;
	ring->vm_hub = vm_hub;
	ring->no_scheduler = false;
	strscpy(ring->name, name, sizeof(ring->name));
	r = amdgpu_ring_init(adev, ring, 1024, &fence_irq, 0, AMDGPU_RING_PRIO_DEFAULT, NULL);
	if (r)
		FX_ABORT("amdgpu_ring_init(%s) = %d", name, r);
	args.credit_limit = ring->num_hw_submission;
	r = drm_sched_init(&ring->sched, &args);
	if (r)
		FX_ABORT("drm_sched_init(%s) = %d", name, r);
	ring->sched.ready = true;
	e->ring = ring;
	pthread_mutex_init(&e->lock, NULL);
	pthread_cond_init(&e->kick, NULL);
	if (pthread_create(&e->thread, NULL, engine_main, e))
		FX_ABORT("engine thread");
}

struct pci_dev *cs_fixture_init(void)
{
	static struct pci_dev dev;
	static struct pci_bus bus = { .number = 0xc3 };
	static u64 dma_mask = DMA_BIT_MASK(64);
	int r;

	/* What linuxu_driver_bootstrap runs before the amdgpu module. */
	if (linuxu_sysinfo_init() || linuxu_timer_service_init() || linuxu_workqueue_init() ||
	    linuxu_module_init_gpu_buddy_module_init() || linuxu_module_init_drm_core_init() ||
	    linuxu_module_init_drm_sched_fence_slab_init() || amdgpu_sync_init())
		FX_ABORT("module init");

	/* linuxu_driver_bootstrap's policy: no GPU recovery, a job timeout
	 * only signals -ETIME. */
	amdgpu_gpu_recovery = 0;

	/* The PCI function: BAR0 VRAM aperture, BAR2 doorbells, BAR5 MMIO. */
	pdev = &dev;
	pdev->bus = &bus;
	pdev->dev.bus = &pci_bus_type;
	pdev->dev.dma_mask = &dma_mask;
	pdev->dev.coherent_dma_mask = dma_mask;
	device_initialize(&pdev->dev);
	if (dev_set_name(&pdev->dev, "0000:c3:00.0") || device_add(&pdev->dev))
		FX_ABORT("PCI device");
	pdev->vendor = PCI_VENDOR_ID_ATI;
	pdev->device = 0x7551;
	pdev->revision = 0xc0;
	pdev->class = PCI_CLASS_DISPLAY_VGA << 8;
	pdev->resource[0] = (struct resource){ .start = FX_BAR0,
		.end = FX_BAR0 + FX_VRAM_BYTES - 1, .flags = IORESOURCE_MEM };
	pdev->resource[2] = (struct resource){ .start = FX_BAR2,
		.end = FX_BAR2 + FX_BAR2_BYTES - 1, .flags = IORESOURCE_MEM };
	pdev->resource[5] = (struct resource){ .start = FX_BAR5,
		.end = FX_BAR5 + (512 << 10) - 1, .flags = IORESOURCE_MEM };
	/* Host test DMA addresses are user pointers: no narrower mask (as
	 * rt_device_alloc does for the host backend). */

	/* A display (cs_fixture.h): modesetting and dumb buffers. */
	if (cs_fixture_display) {
		fx_driver.driver_features |= DRIVER_MODESET | DRIVER_ATOMIC;
		fx_driver.dumb_create = amdgpu_mode_dumb_create;
	}
	adev = devm_drm_dev_alloc(&pdev->dev, &fx_driver, typeof(*adev), ddev);
	if (IS_ERR(adev))
		FX_ABORT("devm_drm_dev_alloc = %ld", PTR_ERR(adev));
	adev->dev = &pdev->dev;
	adev->pdev = pdev;
	pci_set_drvdata(pdev, adev_to_drm(adev));

	/* amdgpu_device_init's software state. */
	adev->asic_type = CHIP_IP_DISCOVERY;
	adev->family = AMDGPU_FAMILY_GC_12_0_0;
	adev->ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	adev->ip_versions[SDMA0_HWIP][0] = IP_VERSION(7, 0, 1);
	adev->usec_timeout = AMDGPU_MAX_USEC_TIMEOUT;
	adev->gmc.gart_size = FX_GART_BYTES;
	RCU_INIT_POINTER(adev->gang_submit, dma_fence_get_stub());
	adev->fence_context = dma_fence_context_alloc(AMDGPU_MAX_RINGS);
	mutex_init(&adev->firmware.mutex);
	mutex_init(&adev->pm.mutex);
	mutex_init(&adev->gfx.gpu_clock_mutex);
	mutex_init(&adev->srbm_mutex);
	mutex_init(&adev->gfx.pipe_reserve_mutex);
	mutex_init(&adev->gfx.gfx_off_mutex);
	mutex_init(&adev->gfx.partition_mutex);
	mutex_init(&adev->grbm_idx_mutex);
	mutex_init(&adev->mn_lock);
	mutex_init(&adev->virt.vf_errors.lock);
	hash_init(adev->mn_hash);
	mutex_init(&adev->psp.mutex);
	mutex_init(&adev->notifier_lock);
	mutex_init(&adev->pm.stable_pstate_ctx_lock);
	mutex_init(&adev->benchmark_mutex);
	mutex_init(&adev->gfx.reset_sem_mutex);
	mutex_init(&adev->enforce_isolation_mutex);
	for (int i = 0; i < MAX_XCP; ++i) {
		adev->isolation[i].spearhead = dma_fence_get_stub();
		amdgpu_sync_create(&adev->isolation[i].active);
		amdgpu_sync_create(&adev->isolation[i].prev);
	}
	mutex_init(&adev->gfx.userq_sch_mutex);
	mutex_init(&adev->gfx.workload_profile_mutex);
	mutex_init(&adev->vcn.workload_profile_mutex);
	spin_lock_init(&adev->mmio_idx_lock);
	spin_lock_init(&adev->mm_stats.lock);
	spin_lock_init(&adev->virt.rlcg_reg_lock);
	spin_lock_init(&adev->wb.lock);
	INIT_LIST_HEAD(&adev->reset_list);
	INIT_LIST_HEAD(&adev->ras_list);
	INIT_LIST_HEAD(&adev->pm.od_kobj_list);
	xa_init_flags(&adev->userq_doorbell_xa, XA_FLAGS_LOCK_IRQ);
	INIT_DELAYED_WORK(&adev->delayed_init_work, fx_noop_work);
	adev->reset_domain = amdgpu_reset_create_reset_domain(SINGLE_DEVICE, "amdgpu-reset-dev");
	if (!adev->reset_domain)
		FX_ABORT("reset domain");
	amdgpu_set_init_level(adev, AMDGPU_INIT_LEVEL_DEFAULT);
	adev->gfx_timeout = adev->compute_timeout = adev->sdma_timeout =
		adev->video_timeout = msecs_to_jiffies(10000);
	adev->asic_funcs = &fx_asic_funcs;
	adev->gfx.funcs = &fx_gfx_funcs;
	adev->gfx.rlc.funcs = &fx_rlc_funcs;
	adev->hdp.funcs = &fx_hdp_funcs;
	adev->ip_blocks[0].version = &fx_gfx_block;
	adev->ip_blocks[0].adev = adev;
	adev->ip_blocks[0].status.valid = true;
	adev->ip_blocks[1].version = &fx_sdma_block;
	adev->ip_blocks[1].adev = adev;
	adev->ip_blocks[1].status.valid = true;
	adev->num_ip_blocks = 2;
	fx_gfx_config();

	/* The GMC's sw_init: memory layout, VM sizes, the GART table. */
	vram = calloc(1, FX_VRAM_BYTES);
	doorbells = calloc(1, FX_BAR2_BYTES);
	if (!vram || !doorbells)
		FX_ABORT("fixture memory");
	adev->gmc.gmc_funcs = &fx_gmc_funcs;
	adev->gmc.mc_vram_size = adev->gmc.real_vram_size = FX_VRAM_BYTES;
	/* A BAR narrower than VRAM when asked (the iPad's is 256 MiB of
	 * 32 GiB): TTM then cannot fall back to a CPU copy of invisible VRAM. */
	adev->gmc.visible_vram_size = cs_fixture_visible_vram ? cs_fixture_visible_vram :
				      FX_VRAM_BYTES;
	adev->gmc.aper_base = FX_BAR0;
	adev->gmc.aper_size = FX_VRAM_BYTES;
	adev->gmc.vram_start = FX_VRAM_START;
	adev->gmc.vram_end = FX_VRAM_START + FX_VRAM_BYTES - 1;
	adev->gmc.fb_start = adev->gmc.vram_start;
	adev->gmc.fb_end = adev->gmc.vram_end;
	adev->gmc.gart_start = FX_GART_START;
	adev->gmc.gart_end = FX_GART_START + FX_GART_BYTES - 1;
	adev->gmc.vram_type = AMDGPU_VRAM_TYPE_GDDR6;
	adev->gmc.vram_width = 256;
	adev->vm_manager.vram_base_offset = adev->gmc.vram_start;
	adev->vm_manager.first_kfd_vmid = 8;
	set_bit(AMDGPU_GFXHUB(0), adev->vmhubs_mask);
	set_bit(AMDGPU_MMHUB0(0), adev->vmhubs_mask);
	adev->doorbell.base = FX_BAR2;
	adev->doorbell.size = FX_BAR2_BYTES;
	/* The software rings take no doorbells; host builds cannot ioremap
	 * the doorbell BAR for a kernel doorbell page either. */
	adev->doorbell.num_kernel_doorbells = 0;
	amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48);
	/* amdgpu_bo_init, then the GART: its table comes after TTM, and
	 * enabling it rebinds what TTM bound before (amdgpu_gtt_mgr_recover). */
	r = amdgpu_ttm_init(adev);
	if (r)
		FX_ABORT("amdgpu_ttm_init = %d", r);
	adev->mman.aper_base_kaddr = vram;
	r = amdgpu_gart_init(adev);
	if (!r) {
		/* As gmc_v12_0_gart_init: the table in VRAM. */
		adev->gart.table_size = adev->gart.num_gpu_pages * 8;
		adev->gart.gart_pte_flags = 0;
		r = amdgpu_gart_table_vram_alloc(adev);
	}
	if (r)
		FX_ABORT("GART: %d", r);
	amdgpu_gtt_mgr_recover(&adev->mman.gtt_mgr);
	amdgpu_vm_manager_init(adev);

	/* The GMC's hw_init tail: write-back slots and seq64. */
	r = amdgpu_bo_create_kernel(adev, AMDGPU_MAX_WB * sizeof(uint32_t) * 8, PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_GTT, &adev->wb.wb_obj, &adev->wb.gpu_addr,
				    (void **)&adev->wb.wb);
	if (r)
		FX_ABORT("write-back BO = %d", r);
	adev->wb.num_wb = AMDGPU_MAX_WB;
	memset(&adev->wb.used, 0, sizeof(adev->wb.used));
	memset((char *)adev->wb.wb, 0, AMDGPU_MAX_WB * sizeof(uint32_t) * 8);
	r = amdgpu_seq64_init(adev);
	if (r)
		FX_ABORT("seq64 = %d", r);
	r = amdgpu_ib_pool_init(adev);
	if (r)
		FX_ABORT("IB pool = %d", r);

	/* GFX and SDMA sw_init: their rings on the fence interrupt source. */
	adev->irq.installed = true;
	fence_irq.num_types = 1;
	fence_irq.funcs = &fx_irq_funcs;
	fence_irq.enabled_types = kcalloc(1, sizeof(atomic_t), GFP_KERNEL);
	adev->gfx.num_compute_rings = 1;
	adev->sdma.num_instances = cs_fixture_sdma_instances == 2 ? 2 : 1;
	ring_setup(&adev->gfx.compute_ring[0], &fx_compute_funcs, "comp_1.0.0",
		   AMDGPU_GFXHUB(0), &compute_engine);
	ring_setup(&adev->sdma.instance[0].ring, &fx_sdma_funcs, "sdma0", AMDGPU_GFXHUB(0),
		   &sdma_engine);
	if (adev->sdma.num_instances == 2)
		ring_setup(&adev->sdma.instance[1].ring, &fx_sdma_funcs, "sdma1", AMDGPU_GFXHUB(0),
			   &sdma1_engine);
	adev->mman.buffer_funcs = &fx_buffer_funcs;
	adev->mman.buffer_funcs_ring = &adev->sdma.instance[0].ring;
	adev->vm_manager.vm_pte_funcs = &fx_pte_funcs;
	adev->vm_manager.vm_pte_scheds[0] = &adev->sdma.instance[0].ring.sched;
	adev->vm_manager.vm_pte_num_scheds = 1;
	amdgpu_ttm_set_buffer_funcs_status(adev, true);
	adev->accel_working = true;
	/* As rt_compute_open does for a session: DMA releases wait while an
	 * engine is stalled. */
	if (!getenv("CS_FIXTURE_NO_DMA_HOLD"))	/* the negative control */
		rt_dma_hold_attach(adev);

	/* The display IP's init runs before registration, as in
	 * amdgpu_device_ip_init. */
	if (cs_fixture_display)
		cs_fixture_display(adev);
	r = drm_dev_register(adev_to_drm(adev), 0);
	if (r)
		FX_ABORT("drm_dev_register = %d", r);
	return pdev;
}

void cs_fixture_stop(void)
{
	struct engine *engines[] = { &compute_engine, &sdma_engine, &sdma1_engine };

	for (unsigned int i = 0; i < 3; ++i) {
		if (!engines[i]->ring)
			continue;
		pthread_mutex_lock(&engines[i]->lock);
		engines[i]->stop = true;
		pthread_cond_signal(&engines[i]->kick);
		pthread_mutex_unlock(&engines[i]->lock);
		pthread_join(engines[i]->thread, NULL);
	}
}

void cs_fixture_stats(struct cs_fixture_stats *out)
{
	pthread_mutex_lock(&stats_lock);
	*out = stats;
	pthread_mutex_unlock(&stats_lock);
}

uint8_t *cs_fixture_vram_host(uint64_t mc)
{
	return mc >= adev->gmc.vram_start && mc <= adev->gmc.vram_end ?
		vram + (mc - adev->gmc.vram_start) : NULL;
}

struct amdgpu_device *cs_fixture_adev(void)
{
	return adev;
}

void cs_fixture_hold_sdma(int hold)
{
	pthread_mutex_lock(&sdma_engine.lock);
	sdma_engine.hold = hold;
	pthread_cond_signal(&sdma_engine.kick);
	pthread_mutex_unlock(&sdma_engine.lock);
}

void *cs_fixture_bar_memory(uint32_t bar, uint64_t offset, uint64_t bytes)
{
	/* Only what the BAR shows (cs_fixture_visible_vram narrows it). */
	const uint64_t visible = adev->gmc.visible_vram_size;

	if (bar != 0 || offset > visible || bytes > visible - offset)
		return NULL;
	return vram + offset;
}

void cs_fixture_model_driver_streams(int on)
{
	driver_streams = on;
}

void cs_fixture_hold_compute(int hold)
{
	pthread_mutex_lock(&compute_engine.lock);
	compute_engine.hold = hold;
	pthread_cond_signal(&compute_engine.kick);
	pthread_mutex_unlock(&compute_engine.lock);
}
