/* linuxu shim: iokit_bridge — IOKit <-> linuxu rt glue.
 *
 * dext build (LINUXU_DEXT): the REAL DART/DMA seam.  The linuxu DART layer
 * (linuxu/src/dart/dart.c) calls these to get a GPU-visible IOVA over a
 * coherent buffer via the real DriverKit IODMACommand /
 * IOBufferMemoryDescriptor.  This is the hardware-verified pattern ported
 * from mac_amdgpu (dext/amdgpu/amdgpu_gart.cpp:185-231):
 *
 *   IOBufferMemoryDescriptor::Create(direction, size, align, &buf)
 *   buf->SetLength(size)
 *   buf->GetAddressRange(&cpu)        -> the in-process (host) pointer
 *   IODMACommand::Create(pci, ..., &spec, &dma)   spec.maxAddressBits =
 *                                        the device's DMA mask width
 *   dma->PrepareForDMA(..., buf, 0, size, &flags, &count, &seg)
 *                                        -> seg.address = the IOVA (GPU addr)
 *   ... (GPU DMAs to/from seg.address; CPU touches cpu.address) ...
 *   dma->CompleteDMA(...);  dma->release();  buf->release();
 *
 * The 1.5 GB DART budget is owned by the linuxu DART layer (dart.c), which
 * charges/refunds around ordinary calls. During shutdown, its frees
 * retire software ownership while this seam keeps the mappings prepared;
 * the hold snapshots the same ceiling and retains their byte charges until
 * reset, so teardown allocations cannot exceed the actual pinned budget.
 *
 * 16 KB host-page granularity: DART-pinned sysmem must be 16 KB aligned
 * (DART page size).  We round the size up and pass the 16 KB
 * alignment to Create so the buffer the dext hands back is 16 KB aligned.
 *
 * host build: the testable twins.  They allocate a real aligned buffer and
 * return identity IOVA (host VA == IOVA, the in-process model), so the
 * linuxu DART layer's host path is byte-identical and unit-testable on the
 * desktop without DriverKit. */
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <rt/dext_dma.h>

#ifdef LINUXU_DEXT

#import <DriverKit/IOService.h>
#import <DriverKit/IOBufferMemoryDescriptor.h>
#import <DriverKit/IODMACommand.h>
#import <DriverKit/IOMemoryMap.h>
#import <DriverKit/IOLib.h>
#import <PCIDriverKit/IOPCIDevice.h>
#include "retained_log.h"

static void dext_dma_log_sink(const char *text) { IOLog("%s", text); }
#define DEXT_DMA_LOG(...) maclinuxgpu::RetainedLog(dext_dma_log_sink, __VA_ARGS__)

/* 16 KB coherent alignment (firmware/ucode load
 * needs it, matching DART_COHERENT_ALIGN in dart.c). */
#define DEXT_DART_COHERENT_ALIGN 0x4000UL

/* The IOPCIDevice the DMA commands bind to.  Wired by dext_set_pci()
 * (dext_main.m) — the same device the MMIO seam uses.  We keep our own
 * pointer so the DMA seam is independent of the MMIO seam's init order. */
static IOPCIDevice *g_dma_pci;
static bool g_dma_stopping;
static bool g_dma_resetting;
static bool g_dma_holding_frees;
static bool g_dma_probe_hold;
static bool g_dma_probe_committing;
static bool g_dma_quarantined;
/* The device left the bus: it can reach no memory, so nothing is held for
 * it and a quarantine no longer applies (dext_dma_device_removed). */
static bool g_dma_removed;
static uint64_t g_dma_shutdown_bytes;
static uint64_t g_dma_shutdown_ceiling;
static bool g_dma_cleanup_failed;
static unsigned int g_dma_operations;
/* DMA addressing width of the bound device, from its Linux DMA masks
 * (dext_dma_set_address_bits). 64 until the device sets a mask: placement is
 * then left to the platform. */
static unsigned int g_dma_address_bits = 64;
/* Cached answers of dext_dma_platform_supports_bits for this provider: the
 * narrowest width the DART was seen to satisfy and the widest it could not. */
static unsigned int g_dma_probe_satisfied = 65;
static unsigned int g_dma_probe_refused;
static bool dext_dma_lock;
static void dext_dma_acquire(void)
{
	while (__atomic_test_and_set(&dext_dma_lock, __ATOMIC_ACQUIRE))
		__asm__ volatile("yield");
}
static void dext_dma_release(void)
{
	__atomic_clear(&dext_dma_lock, __ATOMIC_RELEASE);
}
static bool dext_dma_busy_locked(void);
struct dext_dma_snapshot {
	bool provider, stopping, resetting, hold, probe, committing, quarantined, cleanup_failed;
	bool cpu, vmaps;
	unsigned int operations, bars, live, retired;
	uint64_t bytes, ceiling;
};
static dext_dma_snapshot dext_dma_snapshot_locked(void);
/* Capture only cached ownership under the DMA lock, then format and emit
 * after unlocking. Never query PCI/DART to diagnose a failed transition. */
static void dext_dma_report(const char *stage, const dext_dma_snapshot &state)
{
	DEXT_DMA_LOG("DMA %s: provider=%d stopping=%d resetting=%d hold=%d probe=%d committing=%d quarantined=%d cleanup_failed=%d operations=%u bars=%u cpu=%d vmaps=%d live=%u retired=%u bytes=%llu/%llu",
		stage, state.provider, state.stopping, state.resetting, state.hold,
		state.probe, state.committing, state.quarantined, state.cleanup_failed,
		state.operations, state.bars, state.cpu, state.vmaps, state.live,
		state.retired, (unsigned long long)state.bytes, (unsigned long long)state.ceiling);
}

/* A mapping RPC may outlive the stop request. Keep it visible to fini until
 * either its descriptor is published or its failure cleanup has completed. */
class dext_dma_operation {
private:
	uint64_t reserved = 0;
	bool prepared = false;
public:
	IOPCIDevice *pci;
	dext_dma_operation(const dext_dma_operation &) = delete;
	dext_dma_operation &operator=(const dext_dma_operation &) = delete;
	explicit dext_dma_operation(bool cleanup = false) : pci(nullptr)
	{
		dext_dma_acquire();
		if (g_dma_pci && !g_dma_resetting &&
		    (cleanup || (!g_dma_stopping && !g_dma_cleanup_failed))) {
			pci = g_dma_pci;
			++g_dma_operations;
		}
		dext_dma_release();
	}
	~dext_dma_operation()
	{
		if (!pci) return;
		dext_dma_acquire();
		if (reserved && !prepared) g_dma_shutdown_bytes -= reserved;
		--g_dma_operations;
		dext_dma_release();
	}
	bool reserve_shutdown_bytes(uint64_t bytes)
	{
		dext_dma_acquire();
		if (g_dma_quarantined) { dext_dma_release(); return false; }
		if (g_dma_holding_frees) {
			if (bytes > g_dma_shutdown_ceiling - g_dma_shutdown_bytes) {
				dext_dma_release();
				return false;
			}
			g_dma_shutdown_bytes += bytes;
			reserved = bytes;
		}
		dext_dma_release();
		return true;
	}
	void did_prepare() { prepared = true; }
};

/* CompleteDMA failure does not establish that DART has stopped referencing
 * this backing. Preserve both objects and block further sessions. */
static int dext_dma_complete_now(IODMACommand *dma, IOMemoryDescriptor *buf)
{
	dext_dma_acquire();
	bool quarantined = g_dma_quarantined;
	dext_dma_release();
	if (quarantined) return -1;
	const kern_return_t completed = dma ?
		dma->CompleteDMA(kIODMACommandCompleteDMANoOptions) : kIOReturnSuccess;
	if (completed != kIOReturnSuccess) {
		dext_dma_acquire();
		g_dma_cleanup_failed = true;
		dext_dma_release();
		DEXT_DMA_LOG("DMA completion failed (%#x); backing retained", completed);
		return -1;
	}
	/* A quarantine request may arrive while CompleteDMA is in flight. The
	 * RPC cannot be cancelled, but its return must not release this backing. */
	dext_dma_acquire();
	quarantined = g_dma_quarantined;
	dext_dma_release();
	if (quarantined) return -1;
	if (dma) dma->release();
	if (buf) buf->release();
	return 0;
}

static int dext_dma_complete(IODMACommand *, IOMemoryDescriptor *, uint64_t);

/* Forward declarations: the side-table helpers are defined below but are
 * called from dext_dma_alloc_coherent / dext_dma_free_coherent above them
 * in this file.  (C requires declaration-before-use.) */
static int dext_dma_store(IOMemoryDescriptor *buf, IODMACommand *dma,
			   uint64_t cpu_addr, uint64_t length, uint64_t import_id = 0);
static int dext_dma_reap_obj(uint64_t cpu_addr);
extern "C" int dext_copy_bar_memory(uint8_t bar, uint64_t *size,
					     void **descriptor);
extern "C" int dext_bar0_live_count(void);
extern "C" int dext_dma_live_count(void);
extern "C" int dext_dma_fini(void);
extern "C" int dext_dma_free_coherent(void *cpu_addr, size_t size);

/* extern "C" so the linuxu C DART layer (dart.c) can call these. */
extern "C" {

/* dext_main.m's dext_set_pci hands us the IOPCIDevice too (one setter,
 * two seams).  Returns 0 on success. */
int dext_dma_set_pci(void *pci_device)
{
	if (!pci_device)
		return dext_dma_fini();
	dext_dma_acquire();
	if (dext_dma_busy_locked()) {
		dext_dma_release();
		return -1;
	}
	if (g_dma_pci != pci_device) {
		g_dma_probe_satisfied = 65;
		g_dma_probe_refused = 0;
	}
	g_dma_pci = static_cast<IOPCIDevice *>(pci_device);
	g_dma_stopping = false;
	g_dma_removed = false;	/* a new device starts with nothing removed */
	dext_dma_release();
	return 0;
}

int dext_dma_set_address_bits(unsigned int bits)
{
	if (bits < 32 || bits > 64)
		return -1;
	dext_dma_acquire();
	g_dma_address_bits = bits;
	dext_dma_release();
	return 0;
}

unsigned int dext_dma_address_bits(void)
{
	dext_dma_acquire();
	const unsigned int bits = g_dma_address_bits;
	dext_dma_release();
	return bits;
}

/* A live descriptor may still be referenced by GPU work. The caller must
 * release each mapping after quiescing the GPU; Stop must retain the PCI
 * provider if that has not happened. */
int dext_dma_fini(void)
{
	dext_dma_acquire();
	g_dma_stopping = true;
	if (dext_dma_busy_locked()) {
		const auto snapshot = dext_dma_snapshot_locked();
		dext_dma_release();
		dext_dma_report("fini rejected", snapshot);
		return -1;
	}
	g_dma_pci = nullptr;
	dext_dma_release();
	return 0;
}

} /* extern "C" */

/* Outcomes of one mapping attempt. */
enum dext_dma_map_status {
	kDextDMAMapped = 0,
	kDextDMAFailed = -1,          /* DriverKit or bookkeeping failure */
	kDextDMAPrepareRefused = -2,  /* PrepareForDMA refused the mapping */
	kDextDMAOutOfRange = -3,      /* the DART placed it at or above 2^bits */
};

/* True when [address, address + length) lies entirely below 2^bits. */
static bool dext_dma_below(uint64_t address, uint64_t length, unsigned int bits)
{
	if (!length || length - 1 > UINT64_MAX - address)
		return false;
	return bits >= 64 || ((address + length - 1) >> bits) == 0;
}

/* Allocate and DMA-map one coherent buffer whose IODMACommand is limited to
 * `bits` address bits. A mapping placed outside that range is never
 * published; *placed (optional) reports where the DART put it. */
static int dext_dma_map(size_t size, unsigned int bits, void **cpu_addr, uint64_t *iova,
			uint64_t *placed = nullptr)
{
	uint64_t rounded;
	IOBufferMemoryDescriptor *buf = nullptr;
	IODMACommand              *dma = nullptr;
	IOAddressSegment            seg{};
	uint64_t flags = 0;
	uint32_t count = 1;
	kern_return_t r;

	if (!cpu_addr || !iova || size == 0 ||
	    size > UINT64_MAX - (DEXT_DART_COHERENT_ALIGN - 1))
		return kDextDMAFailed;
	*cpu_addr = nullptr;
	*iova     = 0;
	dext_dma_operation operation;
	if (!operation.pci) return kDextDMAFailed;
	IOPCIDevice *pci = operation.pci;

	/* Round up to 16 KB so the DART-pinned sysmem is 16 KB aligned
	 * (the host page size on Apple Silicon; the GPU DART pages are 4 KB
	 * but the host backing must be a multiple of the host page). */
	rounded = (size + DEXT_DART_COHERENT_ALIGN - 1) &
		  ~(DEXT_DART_COHERENT_ALIGN - 1);
	if (!operation.reserve_shutdown_bytes(rounded)) return kDextDMAFailed;

	/* 1. Allocate the I/O buffer (the dext owns the memory). */
	r = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
					      rounded,
					      DEXT_DART_COHERENT_ALIGN, &buf);
	if (r != kIOReturnSuccess || !buf)
		return kDextDMAFailed;
	r = buf->SetLength(rounded);
	if (r != kIOReturnSuccess) { buf->release(); return kDextDMAFailed; }

	/* 2. The in-process (host) pointer the CPU will use. */
	IOAddressSegment cpu{};
	r = buf->GetAddressRange(&cpu);
	if (r != kIOReturnSuccess || !cpu.address || cpu.length < rounded) {
		buf->release();
		return kDextDMAFailed;
	}

	/* 3. Bind a DMA command to the PCI device, limited to the device's
	 * DMA addressing width (its Linux DMA masks; see dma_mask.c). */
	IODMACommandSpecification spec{};
	spec.options       = kIODMACommandSpecificationNoOptions;
	spec.maxAddressBits = bits;
	r = IODMACommand::Create(pci, kIODMACommandCreateNoOptions,
				 &spec, &dma);
	if (r != kIOReturnSuccess || !dma) { buf->release(); return kDextDMAFailed; }

	/* 4. PrepareForDMA -> the GPU-visible IOVA (segment.address).
	 * For a single contiguous buffer we expect exactly one segment
	 * covering the whole rounded length. */
	r = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
			       rounded, &flags, &count, &seg);
	if (r != kIOReturnSuccess) {
		dma->release(); buf->release();
		return kDextDMAPrepareRefused;
	}
	operation.did_prepare();
	if (placed) *placed = seg.address;
	if (count != 1 || seg.length < rounded || !seg.address) {
		/* An unusable mapping is never published. Failed completion still
		 * retains its backing and permanently blocks the DMA seam. */
		(void)dext_dma_complete(dma, buf, rounded);
		return kDextDMAFailed;
	}
	if (!dext_dma_below(seg.address, rounded, bits)) {
		/* The device cannot reach this IOVA: never publish it. */
		(void)dext_dma_complete(dma, buf, rounded);
		return kDextDMAOutOfRange;
	}

	/* 5. Hand back both addresses.  The caller (dart.c) now owns the
	 * lifetime via dext_dma_free_coherent (which releases buf+dma).
	 * We stow the objects in a side-table keyed by cpu_addr so the
	 * free can find them without the caller threading them. */
	if (dext_dma_store(buf, dma, cpu.address, rounded) != 0) {
		(void)dext_dma_complete(dma, buf, rounded);
		return kDextDMAFailed;
	}
	*cpu_addr = reinterpret_cast<void *>(cpu.address);
	*iova     = seg.address;
	return kDextDMAMapped;
}

static void dext_dma_probe_note(unsigned int bits, bool satisfied)
{
	dext_dma_acquire();
	if (satisfied && bits < g_dma_probe_satisfied) g_dma_probe_satisfied = bits;
	if (!satisfied && bits > g_dma_probe_refused) g_dma_probe_refused = bits;
	dext_dma_release();
}

extern "C" {

/* Allocate a coherent (DMA-mapped) buffer.  On success:
 *   *cpu_addr = the in-process host pointer (CPU reads/writes this)
 *   *iova     = the GPU-visible IOVA (segment.address from PrepareForDMA),
 *               below 2^dext_dma_address_bits()
 * Returns 0 on success, -1 on any DriverKit failure (budget is the
 * caller's responsibility — dart.c has already charged it). */
int dext_dma_alloc_coherent(size_t size, void **cpu_addr, uint64_t *iova)
{
	const unsigned int bits = dext_dma_address_bits();
	uint64_t placed = 0;
	const int status = dext_dma_map(size, bits, cpu_addr, iova, &placed);
	if (status == kDextDMAOutOfRange)
		DEXT_DMA_LOG("DMA mapping refused: the DART placed it at %#llx, above the device's %u-bit DMA mask",
			(unsigned long long)placed, bits);
	return status == kDextDMAMapped ? 0 : -1;
}

/* The DART window is a property of the SoC, published only on the dart
 * node of the device tree, which is outside the PCI provider chain. Ask the
 * mapper instead: map one page-sized buffer limited to `bits` and see
 * whether it lands below 2^bits. A refused prepare is attributed to the
 * width only if an unconstrained mapping succeeds. */
int dext_dma_platform_supports_bits(unsigned int bits)
{
	if (bits > 64) bits = 64;
	if (bits < 32) return 0;
	dext_dma_acquire();
	const bool provider = g_dma_pci != nullptr;
	const int cached = bits >= g_dma_probe_satisfied ? 1 :
		bits <= g_dma_probe_refused ? 0 : -1;
	dext_dma_release();
	if (cached >= 0) return cached;
	if (!provider) return -1;
	void *cpu = nullptr;
	uint64_t iova = 0, placed = 0;
	int status = dext_dma_map(DEXT_DART_COHERENT_ALIGN, bits, &cpu, &iova, &placed);
	if (status == kDextDMAMapped)
		(void)dext_dma_free_coherent(cpu, DEXT_DART_COHERENT_ALIGN);
	if (status == kDextDMAPrepareRefused && bits < 64 &&
	    dext_dma_map(DEXT_DART_COHERENT_ALIGN, 64, &cpu, &iova, &placed) == kDextDMAMapped) {
		(void)dext_dma_free_coherent(cpu, DEXT_DART_COHERENT_ALIGN);
		dext_dma_probe_note(64, true);
		status = kDextDMAOutOfRange;
	}
	if (status != kDextDMAMapped && status != kDextDMAOutOfRange)
		return -1;
	dext_dma_probe_note(bits, status == kDextDMAMapped);
	DEXT_DMA_LOG("DMA: the platform %s place mappings below 2^%u (IOVA %#llx)",
		status == kDextDMAMapped ? "can" : "cannot", bits, (unsigned long long)placed);
	return status == kDextDMAMapped;
}

/* Free a coherent buffer previously allocated by dext_dma_alloc_coherent.
 * Completes the DMA mapping and releases the IODMACommand + buffer.
 * cpu_addr is the pointer dext_dma_alloc_coherent returned. A failed
 * completion returns -1 and retains the mapping and backing. */
int dext_dma_free_coherent(void *cpu_addr, size_t size)
{
	(void)size; /* rounded size is the descriptor's own length */
	if (!cpu_addr)
		return 0;
	return dext_dma_reap_obj((uint64_t)(uintptr_t)cpu_addr);
}

/* ---- side-table: cpu_addr -> {buf, dma}. Entries stay occupied until
 * CompleteDMA and reference release finish, including concurrent callers. */
struct dext_dma_entry {
	IOMemoryDescriptor       *buf;	/* a coherent buffer, or an imported client descriptor */
	IODMACommand             *dma;
	uint64_t                  cpu_addr;	/* 0 for an import: no CPU mapping here */
	uint64_t                  length;
	int                        in_use;
	bool                       releasing;
	bool                       retired;
	uint64_t                  import_id;	/* nonzero for an import (dext_dma_import) */
};
#define DEXT_DMA_TABLE 4096
static struct dext_dma_entry dext_dma_table[DEXT_DMA_TABLE];

/* Unpublished mappings created during upstream teardown still need the same
 * pin-until-reset rule. A full registry retains the references and latches a
 * cleanup failure instead of completing an untracked mapping prematurely. */
static int dext_dma_complete(IODMACommand *dma, IOMemoryDescriptor *buf,
                             uint64_t length)
{
	dext_dma_acquire();
	if (g_dma_holding_frees) {
		for (auto &entry : dext_dma_table) {
			if (entry.in_use) continue;
			entry = {buf, dma, 0, length, 1, true, true, 0};
			dext_dma_release();
			return 0;
		}
		g_dma_cleanup_failed = true;
		dext_dma_release();
		return -1;
	}
	dext_dma_release();
	return dext_dma_complete_now(dma, buf);
}

/* Return a retained descriptor for the exact coherent allocation. The caller
 * validates its BO ownership before requesting an export. */
void *dext_dma_copy_descriptor(void *cpu_addr)
{
	dext_dma_acquire();
	if (g_dma_quarantined) { dext_dma_release(); return nullptr; }
	IOMemoryDescriptor *found = nullptr;
	for (int i = 0; i < DEXT_DMA_TABLE; ++i) {
		const auto &e = dext_dma_table[i];
		if (e.in_use && !e.releasing && !e.import_id &&
		    e.cpu_addr == (uint64_t)(uintptr_t)cpu_addr) {
			found = e.buf;
			found->retain();
			break;
		}
	}
	dext_dma_release();
	return found;
}

static IOMemoryMap *g_bar0_cpu_map;
static uint64_t g_bar0_cpu_base;
static uint64_t g_bar0_cpu_size;
static uint32_t g_bar0_cpu_refs;

int dext_bar0_live_count(void)
{
	dext_dma_acquire();
	int count = (int)g_bar0_cpu_refs;
	dext_dma_release();
	return count;
}

int dext_bar0_cpu_contains(const void *address, size_t size)
{
	uintptr_t value = (uintptr_t)address;
	dext_dma_acquire();
	int inside = g_bar0_cpu_map && value >= g_bar0_cpu_base &&
		value - g_bar0_cpu_base < g_bar0_cpu_size &&
		size <= g_bar0_cpu_size - (value - g_bar0_cpu_base);
	dext_dma_release();
	return inside;
}

void *dext_bar0_cpu_map(uint64_t offset, uint64_t size)
{
	if (!size || size > UINT64_MAX - offset)
		return nullptr;
	dext_dma_operation operation;
	if (!operation.pci) return nullptr;
	dext_dma_acquire();
	if (g_dma_stopping || g_dma_cleanup_failed) {
		dext_dma_release();
		return nullptr;
	}
	if (g_bar0_cpu_map) {
		if (offset > g_bar0_cpu_size || size > g_bar0_cpu_size - offset ||
		    g_bar0_cpu_refs == UINT32_MAX) {
			dext_dma_release();
			return nullptr;
		}
		g_bar0_cpu_refs++;
		void *address = reinterpret_cast<void *>(g_bar0_cpu_base + offset);
		dext_dma_release();
		return address;
	}
	dext_dma_release();

	uint64_t visible_size = 0;
	void *opaque = nullptr;
	if (dext_copy_bar_memory(0, &visible_size, &opaque) != 0 || !opaque)
		return nullptr;
	auto *memory = static_cast<IOMemoryDescriptor *>(opaque);
	if (offset > visible_size || size > visible_size - offset) {
		memory->release();
		return nullptr;
	}
	IOMemoryMap *map = nullptr;
	/* Device memory stays uncached; upstream CPU stores must reach the BAR. */
	kern_return_t r = memory->CreateMapping(kIOMemoryMapCacheModeInhibit,
					    0, 0, visible_size, 0, &map);
	memory->release();
	if (r != kIOReturnSuccess || !map || !map->GetAddress() ||
	    map->GetLength() < visible_size) {
		if (map) map->release();
		return nullptr;
	}
	dext_dma_acquire();
	if (g_dma_stopping || g_dma_cleanup_failed) {
		dext_dma_release();
		map->release();
		return nullptr;
	}
	if (g_bar0_cpu_map) {
		if (offset > g_bar0_cpu_size || size > g_bar0_cpu_size - offset ||
		    g_bar0_cpu_refs == UINT32_MAX) {
			dext_dma_release();
			map->release();
			return nullptr;
		}
		g_bar0_cpu_refs++;
		void *address = reinterpret_cast<void *>(g_bar0_cpu_base + offset);
		dext_dma_release();
		map->release();
		return address;
	}
	g_bar0_cpu_map = map;
	g_bar0_cpu_base = map->GetAddress();
	g_bar0_cpu_size = visible_size;
	g_bar0_cpu_refs = 1;
	void *address = reinterpret_cast<void *>(g_bar0_cpu_base + offset);
	dext_dma_release();
	return address;
}

int dext_bar0_cpu_unmap(const void *address)
{
	dext_dma_operation operation(true);
	uintptr_t value = (uintptr_t)address;
	dext_dma_acquire();
	if (g_dma_quarantined) { dext_dma_release(); return -1; }
	if (!g_bar0_cpu_map || value < g_bar0_cpu_base ||
	    value - g_bar0_cpu_base >= g_bar0_cpu_size || !g_bar0_cpu_refs) {
		dext_dma_release();
		return 0;
	}
	IOMemoryMap *map = nullptr;
	if (--g_bar0_cpu_refs == 0) {
		map = g_bar0_cpu_map;
		g_bar0_cpu_map = nullptr;
		g_bar0_cpu_base = 0;
		g_bar0_cpu_size = 0;
	}
	dext_dma_release();
	if (map) map->release();
	return 1;
}

struct dext_vmap_entry {
	dext_vmap_entry *next;
	IOMemoryMap *map;
	uint64_t address;
};
static dext_vmap_entry *dext_vmaps;

/* Resident shmem needs descriptor backing for CPU aliases, but does not need
 * an IODMACommand or consume GPU IOVA space until explicitly DMA mapped. */
struct dext_cpu_entry {
	dext_cpu_entry *next;
	IOBufferMemoryDescriptor *buffer;
	uint64_t address, length;
};
static dext_cpu_entry *dext_cpu_buffers;

void *dext_cpu_alloc_pages(size_t size)
{
	if (!size || (size & (DEXT_DART_COHERENT_ALIGN - 1))) return nullptr;
	dext_dma_operation operation;
	if (!operation.pci) return nullptr;
	auto *entry = static_cast<dext_cpu_entry *>(IOMalloc(sizeof(dext_cpu_entry)));
	if (!entry) return nullptr;
	IOBufferMemoryDescriptor *buffer = nullptr;
	IOAddressSegment address{};
	kern_return_t result = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
		size, DEXT_DART_COHERENT_ALIGN, &buffer);
	if (result != kIOReturnSuccess || !buffer ||
		buffer->SetLength(size) != kIOReturnSuccess ||
		buffer->GetAddressRange(&address) != kIOReturnSuccess || !address.address ||
		(address.address & (DEXT_DART_COHERENT_ALIGN - 1)) || address.length < size) {
		if (buffer) buffer->release();
		IOFree(entry, sizeof(*entry));
		return nullptr;
	}
	*entry = {nullptr, buffer, address.address, size};
	dext_dma_acquire();
	if (g_dma_stopping || g_dma_cleanup_failed) {
		dext_dma_release();
		buffer->release();
		IOFree(entry, sizeof(*entry));
		return nullptr;
	}
	entry->next = dext_cpu_buffers;
	dext_cpu_buffers = entry;
	dext_dma_release();
	return reinterpret_cast<void *>(address.address);
}

int dext_cpu_free_pages(void *cpu_addr, size_t size)
{
	if (!cpu_addr) return 0;
	dext_dma_operation operation(true);
	dext_dma_acquire();
	if (g_dma_quarantined) { dext_dma_release(); return -1; }
	dext_cpu_entry **link = &dext_cpu_buffers;
	while (*link && ((*link)->address != (uint64_t)(uintptr_t)cpu_addr || (*link)->length != size))
		link = &(*link)->next;
	dext_cpu_entry *entry = *link;
	if (entry) *link = entry->next;
	dext_dma_release();
	if (!entry) return -1;
	entry->buffer->release();
	IOFree(entry, sizeof(*entry));
	return 0;
}

static bool dext_dma_busy_locked(void)
{
	if (g_dma_operations || g_dma_cleanup_failed || g_dma_holding_frees ||
	    g_bar0_cpu_refs || dext_vmaps || dext_cpu_buffers)
		return true;
	for (int i = 0; i < DEXT_DMA_TABLE; ++i)
		if (dext_dma_table[i].in_use) return true;
	return false;
}

static dext_dma_snapshot dext_dma_snapshot_locked(void)
{
	dext_dma_snapshot snapshot{};
	snapshot.provider = g_dma_pci != nullptr;
	snapshot.stopping = g_dma_stopping;
	snapshot.resetting = g_dma_resetting;
	snapshot.hold = g_dma_holding_frees;
	snapshot.probe = g_dma_probe_hold;
	snapshot.committing = g_dma_probe_committing;
	snapshot.quarantined = g_dma_quarantined;
	snapshot.cleanup_failed = g_dma_cleanup_failed;
	snapshot.operations = g_dma_operations;
	snapshot.bars = g_bar0_cpu_refs;
	snapshot.cpu = dext_cpu_buffers != nullptr;
	snapshot.vmaps = dext_vmaps != nullptr;
	snapshot.bytes = g_dma_shutdown_bytes;
	snapshot.ceiling = g_dma_shutdown_ceiling;
	for (const auto &entry : dext_dma_table) {
		if (!entry.in_use) continue;
		if (entry.retired) ++snapshot.retired;
		else ++snapshot.live;
	}
	return snapshot;
}

extern "C" int dext_dma_begin_reset(void)
{
	dext_dma_acquire();
	if (!g_dma_pci || g_dma_stopping || g_dma_resetting || dext_dma_busy_locked()) {
		dext_dma_release();
		return -1;
	}
	g_dma_resetting = true;
	++g_dma_operations;
	dext_dma_release();
	return 0;
}

extern "C" void dext_dma_end_reset(void)
{
	dext_dma_acquire();
	if (g_dma_resetting) {
		g_dma_resetting = false;
		--g_dma_operations;
	}
	dext_dma_release();
}

/* Called on the serialized owner queue before stopping compute or upstream.
 * Software may relinquish ownership, but DART keeps every backing pinned
 * until upstream workers/IRQs are drained and FLR has isolated the endpoint. */
#ifndef DEXT_DMA_HOLD_DRAIN_MS
#define DEXT_DMA_HOLD_DRAIN_MS 2000
#endif
static int dext_dma_begin_hold(uint64_t dma_budget, bool probe)
{
	dext_dma_acquire();
	/* An operation in flight on another thread is not a reason to give up:
	 * a session closes while work queued by the processes that just exited
	 * (a Linux-file client's KFD release, TTM's delayed frees) is still
	 * unmapping. Each operation is short; wait for none to be in flight,
	 * bounded, before deciding. Operations that begin after the hold are
	 * covered by it. */
	if (g_dma_operations && dma_budget && g_dma_pci && !g_dma_stopping &&
	    !g_dma_resetting && !g_dma_holding_frees && !g_dma_cleanup_failed) {
		const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) +
			(uint64_t)DEXT_DMA_HOLD_DRAIN_MS * 1000000ULL;
		while (g_dma_operations && !g_dma_resetting &&
		       clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < deadline) {
			dext_dma_release();
			IOSleep(1);
			dext_dma_acquire();
		}
	}
	if (!probe && g_dma_probe_hold && !g_dma_quarantined &&
	    !g_dma_probe_committing && !g_dma_resetting && !g_dma_operations &&
	    !g_dma_cleanup_failed) {
		g_dma_probe_hold = false;
		dext_dma_release();
		return 0;
	}
	if (!dma_budget || !g_dma_pci || g_dma_stopping || g_dma_resetting ||
	    g_dma_holding_frees || g_dma_cleanup_failed || g_dma_operations) {
		const auto snapshot = dext_dma_snapshot_locked();
		dext_dma_release();
		dext_dma_report(probe ? "probe hold rejected" : "shutdown hold rejected", snapshot);
		return -1;
	}
	uint64_t total = 0;
	for (const auto &entry : dext_dma_table) {
		if (!entry.in_use) continue;
		if (entry.length > dma_budget - total) {
			const auto snapshot = dext_dma_snapshot_locked();
			dext_dma_release();
			dext_dma_report(probe ? "probe hold exceeds budget" : "shutdown hold exceeds budget", snapshot);
			return -1;
		}
		total += entry.length;
	}
	g_dma_shutdown_ceiling = dma_budget;
	g_dma_shutdown_bytes = total;
	g_dma_holding_frees = true;
	g_dma_probe_hold = probe;
	dext_dma_release();
	return 0;
}

extern "C" int dext_dma_begin_shutdown(uint64_t dma_budget)
{
	return dext_dma_begin_hold(dma_budget, false);
}

extern "C" int dext_dma_begin_probe(uint64_t dma_budget)
{
	return dext_dma_begin_hold(dma_budget, true);
}

extern "C" void dext_dma_quarantine(void)
{
	dext_dma_acquire();
	if (g_dma_removed) {
		/* Nothing on the bus can use the backing any more. */
		dext_dma_release();
		return;
	}
	g_dma_quarantined = true;
	g_dma_stopping = true;
	g_dma_holding_frees = true;
	g_dma_probe_hold = false;
	dext_dma_release();
}

/* Every remaining table entry is a retired descriptor that no software owner
 * can reach, and no operation, CPU alias, CPU-only buffer or failed
 * completion is outstanding. BAR0 CPU mappings are checked by callers. */
static bool dext_dma_owners_released_locked(void)
{
	if (g_dma_operations || g_dma_cleanup_failed || g_dma_resetting ||
	    g_dma_probe_committing || dext_vmaps || dext_cpu_buffers)
		return false;
	for (const auto &entry : dext_dma_table)
		if (entry.in_use && !entry.retired) return false;
	return true;
}

/* Upstream removal ends every Linux owner of the device, but the pinned
 * amdgpu leaves its visible-VRAM aperture ioremapped after drm_dev_unplug()
 * (amdgpu_ttm_fini() only unmaps it inside drm_dev_enter(), and
 * amdgpu_device_unmap_mmio() runs only for a disconnected device). Once all
 * DMA owners and other CPU aliases are gone, no reference to the device
 * remains that could use that mapping, so the lifecycle owner releases it
 * before the endpoint reset instead of treating it as a live user. */
extern "C" int dext_bar0_cpu_release_orphaned(void)
{
	dext_dma_acquire();
	if (!g_bar0_cpu_map) {
		dext_dma_release();
		return 0;
	}
	if (!g_dma_holding_frees || g_dma_quarantined ||
	    !dext_dma_owners_released_locked()) {
		const auto snapshot = dext_dma_snapshot_locked();
		dext_dma_release();
		dext_dma_report("orphaned BAR0 alias release rejected", snapshot);
		return -1;
	}
	IOMemoryMap *map = g_bar0_cpu_map;
	const int references = g_bar0_cpu_refs > INT32_MAX ? INT32_MAX : (int)g_bar0_cpu_refs;
	g_bar0_cpu_map = nullptr;
	g_bar0_cpu_base = 0;
	g_bar0_cpu_size = 0;
	g_bar0_cpu_refs = 0;
	dext_dma_release();
	map->release();
	return references;
}

/* Cached ownership only: whether a quarantined DMA seam holds nothing but
 * retired descriptors, so a verified endpoint reset could release them. A
 * BAR0 CPU mapping alone does not block this: the caller has proven upstream
 * removal and releases it as orphaned once the quarantine is lifted. */
extern "C" int dext_dma_quarantine_releasable(void)
{
	dext_dma_acquire();
	const bool releasable = g_dma_quarantined &&
		dext_dma_owners_released_locked();
	dext_dma_release();
	return releasable;
}

/* Return a quiescent quarantined seam to the shutdown hold. With keep_hold,
 * retired descriptors stay pinned for dext_pci_shutdown_reset(); without it
 * (no PCI session to reset) nothing may remain in the table at all. */
extern "C" int dext_dma_lift_quarantine(int keep_hold)
{
	dext_dma_acquire();
	bool occupied = false;
	for (const auto &entry : dext_dma_table)
		if (entry.in_use) occupied = true;
	if (!g_dma_quarantined || !dext_dma_owners_released_locked() ||
	    (keep_hold ? !g_dma_pci : occupied || g_bar0_cpu_refs)) {
		const auto snapshot = dext_dma_snapshot_locked();
		dext_dma_release();
		dext_dma_report("quarantine release rejected", snapshot);
		return -1;
	}
	g_dma_quarantined = false;
	g_dma_probe_hold = false;
	g_dma_stopping = true;
	g_dma_holding_frees = true;
	if (!keep_hold) {
		g_dma_holding_frees = false;
		g_dma_shutdown_bytes = 0;
		g_dma_shutdown_ceiling = 0;
	}
	dext_dma_release();
	return 0;
}

extern "C" int dext_dma_begin_shutdown_reset(void)
{
	dext_dma_acquire();
	if (!g_dma_pci || !g_dma_holding_frees || g_dma_quarantined || g_dma_resetting ||
	    g_dma_operations || g_dma_cleanup_failed || g_bar0_cpu_refs ||
	    dext_vmaps || dext_cpu_buffers) {
		const auto snapshot = dext_dma_snapshot_locked();
		dext_dma_release();
		dext_dma_report("shutdown reset rejected", snapshot);
		return -1;
	}
	for (const auto &entry : dext_dma_table) {
		if (entry.in_use && !entry.retired) {
			const auto snapshot = dext_dma_snapshot_locked();
			dext_dma_release();
			dext_dma_report("shutdown reset has live owner", snapshot);
			return -1;
		}
	}
	g_dma_stopping = true;
	g_dma_resetting = true;
	++g_dma_operations;
	dext_dma_release();
	return 0;
}

/* The device left the bus. No DART mapping can be used by it any more:
 * lift a quarantine and the shutdown or probe hold, and complete every
 * retired descriptor now. Later frees complete at once. Descriptors whose
 * completion fails stay retained (dext_dma_fini then reports them); the
 * provider can still be closed. Returns how many stayed retained. */
extern "C" int dext_dma_device_removed(void)
{
	dext_dma_acquire();
	g_dma_removed = true;
	g_dma_quarantined = false;
	g_dma_probe_hold = false;
	g_dma_stopping = true;
	dext_dma_release();
	int kept = 0;
	for (auto &entry : dext_dma_table) {
		dext_dma_acquire();
		const bool retired = entry.in_use && entry.retired && !entry.releasing;
		IODMACommand *dma = entry.dma;
		IOMemoryDescriptor *buf = entry.buf;
		if (retired) entry.releasing = true;
		dext_dma_release();
		if (!retired) continue;
		if (dext_dma_complete_now(dma, buf)) {
			++kept;
			continue;
		}
		dext_dma_acquire();
		entry = {};
		dext_dma_release();
	}
	dext_dma_acquire();
	g_dma_holding_frees = false;
	g_dma_shutdown_bytes = 0;
	g_dma_shutdown_ceiling = 0;
	dext_dma_release();
	return kept;
}

extern "C" int dext_dma_end_shutdown_reset(int reset_succeeded)
{
	dext_dma_acquire();
	if (!g_dma_resetting || !g_dma_holding_frees) {
		dext_dma_release();
		return -1;
	}
	dext_dma_release();
	int result = reset_succeeded ? 0 : -1;
	if (!result) {
		for (auto &entry : dext_dma_table) {
			dext_dma_acquire();
			if (g_dma_quarantined) {
				dext_dma_release();
				result = -1;
				break;
			}
			bool retired = entry.in_use && entry.retired;
			IODMACommand *dma = entry.dma;
			IOMemoryDescriptor *buf = entry.buf;
			dext_dma_release();
			if (!retired) continue;
			if (dext_dma_complete_now(dma, buf)) {
				result = -1;
				break;
			}
			dext_dma_acquire();
			entry = {};
			dext_dma_release();
		}
	}
	dext_dma_acquire();
	if (g_dma_quarantined) result = -1;
	if (!result) {
		g_dma_holding_frees = false;
		g_dma_shutdown_bytes = 0;
		g_dma_shutdown_ceiling = 0;
	}
	g_dma_resetting = false;
	--g_dma_operations;
	dext_dma_release();
	return result;
}

/* A successful probe preserves live owners. Only buffers that upstream
 * already relinquished are completed under its ordinary ownership contract.
 * Failed probes instead promote their hold to shutdown and require FLR. */
extern "C" int dext_dma_commit_probe(void)
{
	dext_dma_acquire();
	if (!g_dma_probe_hold || !g_dma_holding_frees || g_dma_quarantined ||
	    g_dma_probe_committing || g_dma_resetting || g_dma_cleanup_failed) {
		dext_dma_release();
		return -1;
	}
	g_dma_probe_committing = true;
	++g_dma_operations;
	dext_dma_release();
	unsigned waits = 0;
	for (;;) {
		dext_dma_acquire();
		if (g_dma_quarantined || g_dma_cleanup_failed) {
			dext_dma_release();
			break;
		}
		dext_dma_entry *retired = nullptr;
		for (auto &entry : dext_dma_table)
			if (entry.in_use && entry.retired) { retired = &entry; break; }
		if (!retired && g_dma_operations == 1) {
			g_dma_holding_frees = false;
			g_dma_probe_hold = false;
			g_dma_probe_committing = false;
			g_dma_shutdown_bytes = g_dma_shutdown_ceiling = 0;
			--g_dma_operations;
			dext_dma_release();
			return 0;
		}
		IODMACommand *dma = retired ? retired->dma : nullptr;
		IOMemoryDescriptor *buf = retired ? retired->buf : nullptr;
		uint64_t bytes = retired ? retired->length : 0;
		dext_dma_release();
		if (!retired) {
			if (waits++ == 1000) break;
			IOSleep(1);
			continue;
		}
		if (dext_dma_complete_now(dma, buf)) break;
		dext_dma_acquire();
		*retired = {};
		g_dma_shutdown_bytes -= bytes;
		dext_dma_release();
	}
	dext_dma_quarantine();
	dext_dma_acquire();
	g_dma_probe_committing = false;
	--g_dma_operations;
	dext_dma_release();
	return -1;
}

/* One descriptor over [addresses[i], addresses[i] + lengths[i]) in order,
 * each range inside one dext-owned buffer (a DMA-table allocation or a
 * CPU-only page block). Concatenation is bounded to 32 children per
 * descriptor by the DriverKit API, so larger arrays fold in stages while
 * each intermediate descriptor is retained. Returns a +1 descriptor. */
static IOMemoryDescriptor *dext_dma_concat_ranges(const uint64_t *addresses,
						  const uint64_t *lengths, size_t count)
{
	if (!addresses || !lengths || !count ||
	    count > SIZE_MAX / sizeof(IOMemoryDescriptor *))
		return nullptr;
	auto **descs = static_cast<IOMemoryDescriptor **>(
		IOMalloc(count * sizeof(IOMemoryDescriptor *)));
	if (!descs) return nullptr;
	size_t built = 0;
	for (; built < count; ++built) {
		const uint64_t addr = addresses[built], len = lengths[built];
		IOMemoryDescriptor *buf = nullptr;
		uint64_t offset = 0;
		if (!len) break;
		dext_dma_acquire();
		for (int i = 0; i < DEXT_DMA_TABLE; ++i) {
			auto &e = dext_dma_table[i];
			if (e.in_use && !e.releasing && !e.import_id && addr >= e.cpu_addr &&
			    addr - e.cpu_addr < e.length &&
			    e.length - (addr - e.cpu_addr) >= len) {
				buf = e.buf;
				offset = addr - e.cpu_addr;
				buf->retain();
				break;
			}
		}
		if (!buf) for (auto *entry = dext_cpu_buffers; entry; entry = entry->next) {
			if (addr >= entry->address && addr - entry->address < entry->length &&
				entry->length - (addr - entry->address) >= len) {
				buf = entry->buffer;
				offset = addr - entry->address;
				buf->retain();
				break;
			}
		}
		dext_dma_release();
		if (!buf) break;
		descs[built] = nullptr;
		kern_return_t r = IOMemoryDescriptor::CreateSubMemoryDescriptor(
			kIOMemoryDirectionOutIn, offset, len, buf, &descs[built]);
		buf->release();
		if (r != kIOReturnSuccess || !descs[built]) {
			if (descs[built]) descs[built]->release();
			break;
		}
	}
	if (built != count) {
		for (size_t i = 0; i < built; ++i) descs[i]->release();
		IOFree(descs, count * sizeof(IOMemoryDescriptor *));
		return nullptr;
	}
	size_t active = count;
	while (active > 1) {
		size_t capacity = (active + 31) / 32;
		auto **next_descs = static_cast<IOMemoryDescriptor **>(
			IOMalloc(capacity * sizeof(IOMemoryDescriptor *)));
		if (!next_descs) {
			for (size_t i = 0; i < active; ++i) descs[i]->release();
			IOFree(descs, active * sizeof(IOMemoryDescriptor *));
			return nullptr;
		}
		size_t next = 0;
		for (size_t i = 0; i < active; i += 32) {
			uint32_t n = static_cast<uint32_t>(active - i > 32 ? 32 : active - i);
			IOMemoryDescriptor *parent = nullptr;
			kern_return_t r = IOMemoryDescriptor::CreateWithMemoryDescriptors(
				kIOMemoryDirectionOutIn, n, &descs[i], &parent);
			if (r != kIOReturnSuccess || !parent) {
				if (parent) parent->release();
				for (size_t j = 0; j < next; ++j)
					next_descs[j]->release();
				for (size_t j = 0; j < active; ++j)
					descs[j]->release();
				IOFree(next_descs, capacity * sizeof(IOMemoryDescriptor *));
				IOFree(descs, active * sizeof(IOMemoryDescriptor *));
				return nullptr;
			}
			next_descs[next++] = parent;
		}
		for (size_t i = 0; i < active; ++i) descs[i]->release();
		IOFree(descs, active * sizeof(IOMemoryDescriptor *));
		descs = next_descs;
		active = next;
	}
	IOMemoryDescriptor *root = descs[0];
	IOFree(descs, active * sizeof(IOMemoryDescriptor *));
	return root;
}

/* A client mapping of host pages that are not one allocation (a KFD
 * process's GTT BO): one descriptor over the runs, in BO order. */
void *dext_dma_copy_ranges_descriptor(const uint64_t *addresses, const uint64_t *lengths,
				      size_t count)
{
	dext_dma_acquire();
	const bool quarantined = g_dma_quarantined;
	dext_dma_release();
	if (quarantined) return nullptr;
	return dext_dma_concat_ranges(addresses, lengths, count);
}

/* Build a CPU alias from the original buffers. */
void *dext_dma_vmap_pages(const void *const *pages, size_t count)
{
	if (!pages || !count || count > SIZE_MAX / sizeof(uint64_t) ||
	    count > SIZE_MAX / DEXT_DART_COHERENT_ALIGN)
		return nullptr;
	dext_dma_operation operation;
	if (!operation.pci) return nullptr;
	auto *addresses = static_cast<uint64_t *>(IOMalloc(count * sizeof(uint64_t)));
	auto *lengths = static_cast<uint64_t *>(IOMalloc(count * sizeof(uint64_t)));
	if (!addresses || !lengths) {
		if (addresses) IOFree(addresses, count * sizeof(uint64_t));
		if (lengths) IOFree(lengths, count * sizeof(uint64_t));
		return nullptr;
	}
	for (size_t i = 0; i < count; ++i) {
		addresses[i] = (uint64_t)(uintptr_t)pages[i];
		lengths[i] = DEXT_DART_COHERENT_ALIGN;
	}
	IOMemoryDescriptor *root = dext_dma_concat_ranges(addresses, lengths, count);
	IOFree(addresses, count * sizeof(uint64_t));
	IOFree(lengths, count * sizeof(uint64_t));
	if (!root) return nullptr;
	IOMemoryMap *map = nullptr;
	kern_return_t r = root->CreateMapping(0, 0, 0, 0, 0, &map);
	root->release();
	if (r != kIOReturnSuccess || !map ||
	    map->GetLength() < count * DEXT_DART_COHERENT_ALIGN) {
		if (map) map->release();
		return nullptr;
	}
	uint64_t address = map->GetAddress();
	auto *entry = static_cast<dext_vmap_entry *>(IOMalloc(sizeof(dext_vmap_entry)));
	if (!address || !entry) {
		if (entry) IOFree(entry, sizeof(*entry));
		map->release();
		return nullptr;
	}
	*entry = {nullptr, map, address};
	dext_dma_acquire();
	if (g_dma_stopping || g_dma_cleanup_failed) {
		dext_dma_release();
		map->release();
		IOFree(entry, sizeof(*entry));
		return nullptr;
	}
	entry->next = dext_vmaps;
	dext_vmaps = entry;
	dext_dma_release();
	return reinterpret_cast<void *>(address);
}

void dext_dma_vunmap_pages(const void *address)
{
	dext_dma_operation operation(true);
	dext_dma_acquire();
	if (g_dma_quarantined) { dext_dma_release(); return; }
	dext_vmap_entry **it = &dext_vmaps;
	while (*it && (*it)->address != (uint64_t)(uintptr_t)address)
		it = &(*it)->next;
	dext_vmap_entry *entry = *it;
	if (entry) *it = entry->next;
	dext_dma_release();
	if (entry) {
		entry->map->release();
		IOFree(entry, sizeof(*entry));
	}
}

static int dext_dma_store(IOMemoryDescriptor *buf, IODMACommand *dma,
			   uint64_t cpu_addr, uint64_t length, uint64_t import_id)
{
	int i;

	dext_dma_acquire();
	if (g_dma_stopping || g_dma_cleanup_failed) {
		dext_dma_release();
		return -1;
	}
	for (i = 0; i < DEXT_DMA_TABLE; i++) {
		if (!dext_dma_table[i].in_use) {
			dext_dma_table[i].buf      = buf;
			dext_dma_table[i].dma      = dma;
			dext_dma_table[i].cpu_addr = cpu_addr;
			dext_dma_table[i].length   = length;
			dext_dma_table[i].in_use   = 1;
			dext_dma_table[i].releasing = false;
			dext_dma_table[i].retired = false;
			dext_dma_table[i].import_id = import_id;
			dext_dma_release();
			return 0;
		}
	}
	dext_dma_release();
	return -1; /* table full: caller releases the objects */
}

static int dext_dma_reap_obj(uint64_t cpu_addr)
{
	int i;
	dext_dma_operation operation(true);
	if (!operation.pci) return -1;

	dext_dma_acquire();
	for (i = 0; i < DEXT_DMA_TABLE; i++) {
		struct dext_dma_entry *e = &dext_dma_table[i];

		if (e->in_use && !e->import_id && e->cpu_addr == cpu_addr) {
			if (e->releasing) {
				dext_dma_release();
				return -1;
			}
			IODMACommand *dma = e->dma;
			IOMemoryDescriptor *buf = e->buf;
			e->releasing = true;
			if (g_dma_holding_frees) {
				e->retired = true;
				int result = g_dma_quarantined ? -1 : 0;
				dext_dma_release();
				return result;
			}
			dext_dma_release();
			if (dext_dma_complete(dma, buf, e->length) != 0)
				return -1;
			dext_dma_acquire();
			e->buf = nullptr;
			e->dma = nullptr;
			e->cpu_addr = 0;
			e->length = 0;
			e->in_use = 0;
			e->releasing = false;
			dext_dma_release();
			return 0;
		}
	}
	dext_dma_release();
	return -1; /* unknown cpu_addr: no-op (idempotent free contract) */
}

/* ---- a client's memory mapped for the device (rt/dext_dma.h) ----
 * The client's memory descriptor (a display agent's IOSurface pages, passed
 * as a structure-input descriptor) is retained and prepared for DMA by an
 * IODMACommand on the bound device, then recorded in the same table as the
 * coherent mappings: the shutdown hold retires its release until the
 * endpoint reset, a quarantine retains it, and fini refuses while it lives. */
static uint64_t g_dma_next_import = 1;

int dext_dma_import(void *descriptor, uint64_t length, uint64_t *addresses,
		    uint64_t *lengths, uint32_t *count, uint64_t *import_id)
{
	if (!descriptor || !length || (length & (DEXT_DART_COHERENT_ALIGN - 1)) ||
	    !addresses || !lengths || !count || !*count || *count > DEXT_DMA_IMPORT_SEGMENTS_MAX ||
	    !import_id)
		return -1;
	*import_id = 0;
	auto *memory = static_cast<IOMemoryDescriptor *>(descriptor);
	dext_dma_operation operation;
	if (!operation.pci) return -1;
	if (!operation.reserve_shutdown_bytes(length)) return -1;
	const unsigned int bits = dext_dma_address_bits();
	IODMACommandSpecification spec{};
	spec.options = kIODMACommandSpecificationNoOptions;
	spec.maxAddressBits = bits;
	IODMACommand *dma = nullptr;
	if (IODMACommand::Create(operation.pci, kIODMACommandCreateNoOptions, &spec, &dma) !=
	    kIOReturnSuccess || !dma)
		return -1;
	IOAddressSegment segments[DEXT_DMA_IMPORT_SEGMENTS_MAX] = {};
	uint32_t n = *count;
	uint64_t flags = 0;
	memory->retain();
	const kern_return_t prepared = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, memory,
							  0, length, &flags, &n, segments);
	if (prepared != kIOReturnSuccess) {
		dma->release();
		memory->release();
		DEXT_DMA_LOG("DMA import refused: PrepareForDMA of %llu bytes failed (%#x)",
			     (unsigned long long)length, prepared);
		return -1;
	}
	operation.did_prepare();
	/* Every segment whole host pages, below the device's reach, and
	 * together exactly the descriptor's length. */
	uint64_t total = 0;
	bool usable = n && n <= *count;
	for (uint32_t i = 0; usable && i < n; ++i) {
		usable = segments[i].address && segments[i].length &&
			!((segments[i].address | segments[i].length) & (DEXT_DART_COHERENT_ALIGN - 1)) &&
			dext_dma_below(segments[i].address, segments[i].length, bits) &&
			segments[i].length <= length - total;
		if (usable) total += segments[i].length;
	}
	if (!usable || total != length) {
		DEXT_DMA_LOG("DMA import refused: %u segment(s) unusable for the device (%u-bit)", n, bits);
		(void)dext_dma_complete(dma, memory, length);
		return -1;
	}
	dext_dma_acquire();
	const uint64_t id = g_dma_next_import++;
	dext_dma_release();
	if (dext_dma_store(memory, dma, 0, length, id) != 0) {
		(void)dext_dma_complete(dma, memory, length);
		return -1;
	}
	for (uint32_t i = 0; i < n; ++i) {
		addresses[i] = segments[i].address;
		lengths[i] = segments[i].length;
	}
	*count = n;
	*import_id = id;
	return 0;
}

int dext_dma_release_import(uint64_t import_id)
{
	if (!import_id) return -1;
	dext_dma_operation operation(true);
	if (!operation.pci) return -1;
	dext_dma_acquire();
	for (auto &e : dext_dma_table) {
		if (!e.in_use || e.import_id != import_id) continue;
		if (e.releasing) { dext_dma_release(); return -1; }
		IODMACommand *dma = e.dma;
		IOMemoryDescriptor *memory = e.buf;
		e.releasing = true;
		if (g_dma_holding_frees) {
			/* Kept mapped until the endpoint reset. */
			e.retired = true;
			const int result = g_dma_quarantined ? -1 : 0;
			dext_dma_release();
			return result;
		}
		const uint64_t length = e.length;
		dext_dma_release();
		if (dext_dma_complete(dma, memory, length) != 0)
			return -1;
		dext_dma_acquire();
		e = {};
		dext_dma_release();
		return 0;
	}
	dext_dma_release();
	return -1;
}

/* Diagnostics for the test/bringup harness. */
int dext_dma_live_count(void)
{
	int i, n = 0;

	dext_dma_acquire();
	for (i = 0; i < DEXT_DMA_TABLE; i++)
		if (dext_dma_table[i].in_use)
			n++;
	dext_dma_release();
	return n;
}

} /* extern "C" */

#else /* host build: identity-IOVA testable twins */
extern "C" int dext_dma_import(void *, uint64_t, uint64_t *, uint64_t *, uint32_t *, uint64_t *) { return -1; }
extern "C" int dext_dma_release_import(uint64_t) { return -1; }
extern "C" int dext_dma_begin_reset(void) { return -1; }
extern "C" void dext_dma_end_reset(void) {}
extern "C" int dext_dma_begin_shutdown(uint64_t) { return -1; }
extern "C" int dext_dma_begin_probe(uint64_t) { return -1; }
extern "C" int dext_dma_commit_probe(void) { return -1; }
extern "C" void dext_dma_quarantine(void) {}
extern "C" int dext_dma_device_removed(void) { return 0; }
extern "C" int dext_dma_begin_shutdown_reset(void) { return -1; }
extern "C" int dext_bar0_cpu_release_orphaned(void) { return 0; }
extern "C" int dext_dma_quarantine_releasable(void) { return 0; }
extern "C" int dext_dma_lift_quarantine(int) { return -1; }
extern "C" int dext_dma_end_shutdown_reset(int) { return -1; }
extern "C" void *dext_cpu_alloc_pages(size_t size)
{
	return size && !(size & (0x4000 - 1)) ? aligned_alloc(0x4000, size) : nullptr;
}
extern "C" int dext_cpu_free_pages(void *address, size_t size)
{
	(void)size;
	free(address);
	return 0;
}


#include <stdlib.h>
#include <rt/dext_dma.h>

extern "C" {

int dext_dma_live_count(void);

/* Host twins: a real 16 KB-aligned anon buffer, identity IOVA (host VA ==
 * IOVA, the in-process model).  dart.c's host path is unchanged — these
 * make the dext_dma_* seam shape available to a host unit test without
 * DriverKit.  The side-table mirrors the dext build's lifetime model. */
#define DEXT_DART_COHERENT_ALIGN 0x4000UL

struct dext_dma_entry {
	void    *buf;
	uint64_t cpu_addr;
	int      in_use;
};
#define DEXT_DMA_TABLE 4096
static struct dext_dma_entry dext_dma_table[DEXT_DMA_TABLE];

int dext_dma_set_pci(void *pci_device)
{
	(void)pci_device;
	return pci_device || dext_dma_live_count() == 0 ? 0 : -1;
}

int dext_dma_fini(void)
{
	return dext_dma_live_count() == 0 ? 0 : -1;
}

/* Identity IOVAs are user-space pointers, which macOS keeps below 2^47. */
#define DEXT_HOST_ADDRESS_BITS 47u
static unsigned int g_host_dma_address_bits = 64;

int dext_dma_set_address_bits(unsigned int bits)
{
	if (bits < 32 || bits > 64)
		return -1;
	g_host_dma_address_bits = bits;
	return 0;
}

unsigned int dext_dma_address_bits(void)
{
	return g_host_dma_address_bits;
}

int dext_dma_platform_supports_bits(unsigned int bits)
{
	return bits >= DEXT_HOST_ADDRESS_BITS;
}

int dext_dma_alloc_coherent(size_t size, void **cpu_addr, uint64_t *iova)
{
	uint64_t rounded;
	void *p;
	int i;

	if (!cpu_addr || !iova || size == 0 ||
	    size > UINT64_MAX - (DEXT_DART_COHERENT_ALIGN - 1))
		return -1;
	*cpu_addr = nullptr;
	*iova     = 0;
	rounded = (size + DEXT_DART_COHERENT_ALIGN - 1) &
		  ~(DEXT_DART_COHERENT_ALIGN - 1);
	p = aligned_alloc(DEXT_DART_COHERENT_ALIGN, rounded);
	if (!p)
		return -1;
	if (g_host_dma_address_bits < 64 &&
	    (((uint64_t)(uintptr_t)p + rounded - 1) >> g_host_dma_address_bits)) {
		free(p); /* beyond the device's DMA mask */
		return -1;
	}
	for (i = 0; i < DEXT_DMA_TABLE; i++) {
		if (!dext_dma_table[i].in_use) {
			dext_dma_table[i].buf      = p;
			dext_dma_table[i].cpu_addr = (uint64_t)(uintptr_t)p;
			dext_dma_table[i].in_use   = 1;
			break;
		}
	}
	if (i == DEXT_DMA_TABLE) { free(p); return -1; }
	/* identity IOVA: the GPU-visible address == the host pointer */
	*cpu_addr = p;
	*iova     = (uint64_t)(uintptr_t)p;
	return 0;
}

int dext_dma_free_coherent(void *cpu_addr, size_t size)
{
	uint64_t a = (uint64_t)(uintptr_t)cpu_addr;
	int i;

	(void)size;
	if (!cpu_addr)
		return 0;
	for (i = 0; i < DEXT_DMA_TABLE; i++) {
		if (dext_dma_table[i].in_use &&
		    dext_dma_table[i].cpu_addr == a) {
			free(dext_dma_table[i].buf);
			dext_dma_table[i].buf      = nullptr;
			dext_dma_table[i].cpu_addr = 0;
			dext_dma_table[i].in_use   = 0;
			return 0;
		}
	}
	return -1;
}

/* (The dext build's internal dext_dma_store / dext_dma_reap_obj are
 * seam-private; the host path's free_coherent does its own table walk, so
 * no host twins are needed here.) */

int dext_dma_live_count(void)
{
	int i, n = 0;

	for (i = 0; i < DEXT_DMA_TABLE; i++)
		if (dext_dma_table[i].in_use)
			n++;
	return n;
}

} /* extern "C" */

#endif /* LINUXU_DEXT */
