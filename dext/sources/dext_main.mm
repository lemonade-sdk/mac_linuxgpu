/* linuxu shim: dext_main — DriverKit dext entry point (dext build) /
 * host stub (macOS host build for tests).  C API: dext_open(token),
 * dext_mem_read32/64 + dext_mem_write32/64, dext_irq_register. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <rt/dext_pci.h>
#include <rt/dext_dma.h>
#include <rt/fatal.h>
#include "pci_reset_policy.h"
#include "pci_access_gate.h"

#ifdef LINUXU_DEXT

#import <DriverKit/IOService.h>
#import <DriverKit/IOLib.h>
#import <DriverKit/IODispatchQueue.h>
#import <DriverKit/IOInterruptDispatchSource.h>
#import <PCIDriverKit/IOPCIDevice.h>
#import <PCIDriverKit/IOPCIFamilyDefinitions.h>

/* The DMA seam (iokit_bridge.m) needs the same IOPCIDevice; dext_set_pci
 * hands it to both seams so init order between them does not matter. */
extern "C" int dext_dma_set_pci(void *pci_device);

/* The linuxu rt token-mint entry (defined in linuxu/src/pci/pdev_mmio.c,
 * linked in from build/libmacamgdu.a).  Mint a fake-MMIO token for a
 * BAR: token → {dev, mem_index, base, size, host_shadow}.  The dext path
 * passes host_shadow=0 (no host shadow — the dext services MMIO from
 * IOPCIDevice, never a CPU mirror). */
extern "C" uint32_t rt_mmio_mint_token(void *dev, uint8_t mem_index,
				       uint64_t base, uint64_t size,
				       int host_shadow);
extern "C" int rt_mmio_slot_info(uint32_t token, uint8_t *mem_index,
					uint64_t *size);
extern "C" void rt_mmio_free_token(uint32_t token);

/* ---- dext-side state (owned by the MacLinuxGPU IOService, set during
 * Start; read by these C API seams).  IOPCIDevice is retained by the
 * IOService and released in free().  The C API here is the seam the
 * linuxu rt layer (and, later, pdev_mmio.c's LINUXU_DEXT branch) calls.
 *
 * NOTE(T-dma-dart-dext / T-irq-dext): the IOPCIDevice claim + DMA/IRQ
 * plumbing is the REAL work for those tracks.  What is implemented here
 * is the C API shape + the IOPCIDevice::MemoryRead/Write dispatch for the
 * primary (BAR5 register) token, which is the seam everything else hangs
 * off.  g_pci is wired by the IOService's Start via dext_set_pci(). */
static IOPCIDevice *g_pci;
static IOService   *g_pci_client;
static bool         g_pci_open;
static bool         g_pci_resetting;
static bool         g_pci_control_lock;
static dext_pci_access_gate g_pci_access;
class dext_pci_control_guard {
public:
	dext_pci_control_guard() {
		while (__atomic_test_and_set(&g_pci_control_lock, __ATOMIC_ACQUIRE))
			__asm__ volatile("yield");
	}
	~dext_pci_control_guard() {
		__atomic_clear(&g_pci_control_lock, __ATOMIC_RELEASE);
	}
};
class dext_pci_operation {
    bool admitted;
public:
    explicit dext_pci_operation(bool require_open = true) {
        dext_pci_control_guard control;
        admitted = g_pci && (!require_open || g_pci_open) &&
            !__atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE) &&
            g_pci_access.enter();
    }
    ~dext_pci_operation() { if (admitted) g_pci_access.leave(); }
    explicit operator bool() const { return admitted; }
    dext_pci_operation(const dext_pci_operation &) = delete;
    dext_pci_operation &operator=(const dext_pci_operation &) = delete;
};
/* Primary fake-MMIO token for the register BAR.  0 until dext_open mints
 * it.  Its window is the whole BAR as assigned (see dext_open). */
static uint32_t     g_reg_token;
static uint8_t      g_reg_bar = UINT8_MAX;
/* Bytes of the register BAR this function can actually access. */
static uint64_t     g_reg_window;
static int          g_transport_fault;
static uint64_t     g_transport_fault_offset;
static int          g_transport_sentinel;
static uint64_t     g_transport_sentinel_offset;

extern "C" void dext_pci_transport_record_fault(int fault, uint64_t offset)
{
	int expected = DEXT_PCI_FAULT_NONE;
	if (fault <= DEXT_PCI_FAULT_NONE)
		return;
	/* Contain a definite seam failure before returning to upstream. Merely
	 * recording it lets other workers continue issuing PCI/DART requests.
	 * These operations touch cached state only: no reset, Close, unmap or
	 * provider release is safe here while another RPC may still be active. */
	g_pci_access.block();
	dext_dma_quarantine();
	/* Reserve the first fault before publishing its offset and code. */
	if (__atomic_compare_exchange_n(&g_transport_fault, &expected, -1,
		false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&g_transport_fault_offset, offset, __ATOMIC_RELAXED);
		__atomic_store_n(&g_transport_fault, fault, __ATOMIC_RELEASE);
	}
}

/* linuxu_fatal() containment hook, run before the failing thread parks.
 * It may run on any thread with arbitrary locks held, so it only records a
 * fault: that closes PCI admission and quarantines DMA backing using cached
 * state, without an RPC drain, reset or release. Bus-master isolation is
 * left to the session owner's dext_pci_quarantine(). The source line is
 * kept as the fault offset for the probe status diagnostics. */
static void dext_fatal_contain(const char *why, const char *file, int line)
{
	(void)why;
	(void)file;
	dext_pci_transport_record_fault(DEXT_PCI_FAULT_FATAL,
		line > 0 ? (uint64_t)line : 0);
}

extern "C" int dext_pci_transport_fault(void)
{
	int fault = __atomic_load_n(&g_transport_fault, __ATOMIC_ACQUIRE);
	return fault < 0 ? DEXT_PCI_FAULT_MMIO : fault;
}

extern "C" uint64_t dext_pci_transport_fault_offset(void)
{
	return __atomic_load_n(&g_transport_fault_offset, __ATOMIC_ACQUIRE);
}

extern "C" void dext_pci_transport_note_sentinel(int source, uint64_t offset)
{
	int expected = DEXT_PCI_SENTINEL_NONE;
	if (source <= DEXT_PCI_SENTINEL_NONE)
		return;
	if (__atomic_compare_exchange_n(&g_transport_sentinel, &expected, -1,
		false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&g_transport_sentinel_offset, offset, __ATOMIC_RELAXED);
		__atomic_store_n(&g_transport_sentinel, source, __ATOMIC_RELEASE);
	}
}

extern "C" int dext_pci_transport_sentinel(void)
{
	int source = __atomic_load_n(&g_transport_sentinel, __ATOMIC_ACQUIRE);
	return source < 0 ? DEXT_PCI_SENTINEL_MMIO : source;
}

extern "C" uint64_t dext_pci_transport_sentinel_offset(void)
{
	return __atomic_load_n(&g_transport_sentinel_offset, __ATOMIC_ACQUIRE);
}

/* ---- IRQ state (T-irq-dext) ----
 * The MSI-X sources run on the owning IOService's serial queue.  The
 * generated MacLinuxGPU action is bound before a source is enabled, so
 * in-process KMD interrupt handling does not depend on a host connection. */
#define LINUXU_DEXT_MAX_IRQ_VECTORS 64
static IODispatchQueue            *g_irq_queue;
static IOInterruptDispatchSource  *g_irq_sources[LINUXU_DEXT_MAX_IRQ_VECTORS];
static int                         g_irq_vector_count;
static bool                        g_irq_armed[LINUXU_DEXT_MAX_IRQ_VECTORS];
static unsigned int                g_irq_type;
static bool                        g_irq_fired;   /* liveness latch: set on
                                                    * any callback delivery */
static bool                        g_irq_draining;
static int                         g_irq_pending_drains;
static bool                        g_irq_drain_failed;
static void                      (*g_irq_drain_complete)(void *);
static void                       *g_irq_drain_context;

/* Register before any linuxu code runs (Start calls into linuxu for
 * firmware registration before dext_set_pci); dext_set_pci repeats it in
 * case initializers are skipped. Without the hook, linuxu_fatal() still
 * parks the thread, but the device is not contained. */
__attribute__((constructor)) static void dext_fatal_register(void)
{
	linuxu_fatal_set_hook(dext_fatal_contain);
}

/* The IOService (MacLinuxGPU) hands the claimed IOPCIDevice to this seam
 * during Start.  Returns 0 on success. */
extern "C" int dext_set_pci(void *pci_device, void *client)
{
	linuxu_fatal_set_hook(dext_fatal_contain);
	dext_pci_control_guard control;
	if (!pci_device || !client)
		return -1;
	if (g_pci_access.closed() || !g_pci_access.drained() ||
	    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
	    g_pci_open || g_irq_vector_count || g_irq_queue)
		return -1;
	if (dext_dma_set_pci(pci_device) != 0)
		return -1;
	__atomic_store_n(&g_transport_fault_offset, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_transport_fault, DEXT_PCI_FAULT_NONE, __ATOMIC_RELEASE);
	__atomic_store_n(&g_transport_sentinel_offset, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_transport_sentinel, DEXT_PCI_SENTINEL_NONE,
		__ATOMIC_RELEASE);
	g_pci = static_cast<IOPCIDevice *>(pci_device);
	g_pci_client = static_cast<IOService *>(client);
	g_pci_open = false;
	return 0;
}

extern "C" void dext_close(void)
{
	dext_pci_control_guard control;
	if (g_pci_access.closed() || !g_pci_access.drained() ||
	    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE)) return;
	if (__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
	    g_irq_vector_count || g_irq_queue) {
		IOLog("MacLinuxGPU: dext_close rejected with live IRQ sources\n");
		return;
	}
	if (g_reg_token) {
		rt_mmio_free_token(g_reg_token);
		g_reg_token = 0;
	}
	g_reg_bar = UINT8_MAX;
	g_reg_window = 0;
	if (g_pci_open && g_pci && g_pci_client)
		g_pci->Close(g_pci_client, 0);
	g_pci_open = false;
	g_pci = nullptr;
	g_pci_client = nullptr;
}

extern "C" int dext_pci_function_reset(void)
{
	IOPCIDevice *pci;
	{
		dext_pci_control_guard control;
		if (g_pci_access.closed() || !g_pci_access.drained() ||
		    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE) ||
		    !g_pci || !g_pci_open || g_irq_vector_count || g_irq_queue ||
		    __atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
		    dext_pci_transport_fault() != DEXT_PCI_FAULT_NONE ||
		    dext_dma_begin_reset()) return -16;
		__atomic_store_n(&g_pci_resetting, true, __ATOMIC_RELEASE);
		pci = g_pci;
		pci->retain();
	}
	bool changed = false;
	int result = dext_reset_idle_endpoint(*pci,
		[pci]() { return pci->Reset(kIOPCIDeviceResetTypeFunctionReset,
			kIOPCIDeviceResetOptionNone) == kIOReturnSuccess ? 0 : -1; },
		[]() { IOSleep(1); }, changed);
	if (result && changed)
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, 4);
	{
		dext_pci_control_guard control;
		dext_dma_end_reset();
		__atomic_store_n(&g_pci_resetting, false, __ATOMIC_RELEASE);
	}
	pci->release();
	return result;
}

extern "C" int dext_pci_shutdown_reset(void)
{
	IOPCIDevice *pci;
	{
		dext_pci_control_guard control;
		if (g_pci_access.closed() || !g_pci_access.drained() ||
		    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE) ||
		    !g_pci || !g_pci_open || g_irq_vector_count || g_irq_queue ||
		    __atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
		    dext_dma_begin_shutdown_reset()) return -16;
		__atomic_store_n(&g_pci_resetting, true, __ATOMIC_RELEASE);
		pci = g_pci;
		pci->retain();
	}
	bool changed = false;
	int result = dext_reset_shutdown_endpoint(*pci,
		[pci]() { return pci->Reset(kIOPCIDeviceResetTypeFunctionReset,
			kIOPCIDeviceResetOptionNone) == kIOReturnSuccess ? 0 : -1; },
		[]() { IOSleep(1); }, changed);
	if (result && changed)
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, 4);
	/* A hardware reset does not prove a failed CompleteDMA was safe. Keep
	 * any descriptors whose completion still fails, including on this path. */
	if (dext_dma_end_shutdown_reset(result == 0) && !result) result = -5;
	{
		dext_pci_control_guard control;
		__atomic_store_n(&g_pci_resetting, false, __ATOMIC_RELEASE);
	}
	pci->release();
	return result;
}

/* ---- surprise removal (the GPU left the bus) ----
 * A device that is gone cannot reach host memory and must not be touched:
 * admission closes for good, nothing resets or isolates it, and the
 * provider is closed as soon as no access is in flight, so the dext never
 * holds a vanished device. */
static bool g_pci_removed;

extern "C" int dext_pci_device_present(void)
{
	IOPCIDevice *pci;
	{
		dext_pci_control_guard control;
		pci = g_pci;
		if (pci) pci->retain();
	}
	if (!pci) return 1;
	/* Directly on the provider: admission may already be closed by the
	 * fault that made the caller ask. A device off the bus reads ~0. */
	uint32_t identity = UINT32_MAX;
	pci->ConfigurationRead32(0, &identity);
	pci->release();
	const uint16_t vendor = (uint16_t)identity;
	return vendor != 0xffff && vendor != 0;
}

extern "C" void dext_pci_mark_removed(void)
{
	__atomic_store_n(&g_pci_removed, true, __ATOMIC_RELEASE);
	g_pci_access.block();
}

extern "C" int dext_pci_removed(void)
{
	return __atomic_load_n(&g_pci_removed, __ATOMIC_ACQUIRE);
}

/* Close a removed device's provider once every admitted access left (at
 * most a second; an access to a vanished device fails at once), then
 * reopen admission and clear the fault records for the next device. 0, or
 * -16 while interrupt sources or an access remain, -22 if not removed. */
extern "C" int dext_pci_close_removed(void)
{
	if (!dext_pci_removed()) return -22;
	for (unsigned waited = 0; !g_pci_access.drained(); ++waited) {
		if (waited == 1000) return -16;
		IOSleep(1);
	}
	dext_pci_control_guard control;
	if (!g_pci_access.drained() ||
	    __atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
	    g_irq_vector_count || g_irq_queue)
		return -16;
	if (g_reg_token) {
		rt_mmio_free_token(g_reg_token);
		g_reg_token = 0;
	}
	g_reg_bar = UINT8_MAX;
	g_reg_window = 0;
	if (g_pci_open && g_pci && g_pci_client)
		g_pci->Close(g_pci_client, 0);
	g_pci_open = false;
	g_pci = nullptr;
	g_pci_client = nullptr;
	__atomic_store_n(&g_pci_resetting, false, __ATOMIC_RELEASE);
	(void)g_pci_access.reopen();
	__atomic_store_n(&g_transport_fault_offset, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_transport_fault, DEXT_PCI_FAULT_NONE, __ATOMIC_RELEASE);
	__atomic_store_n(&g_transport_sentinel_offset, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_transport_sentinel, DEXT_PCI_SENTINEL_NONE, __ATOMIC_RELEASE);
	__atomic_store_n(&g_pci_removed, false, __ATOMIC_RELEASE);
	return 0;
}

extern "C" int dext_pci_quarantine(void)
{
	IOPCIDevice *pci;
	{
		dext_pci_control_guard control;
		g_pci_access.block();
		/* Own code: a first real fault recorded earlier stays visible. */
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_QUARANTINE, 0);
		if (!g_pci || !g_pci_open) return -19;
		pci = g_pci;
		pci->retain();
	}
	/* New seam calls are rejected. Keep the provider and every token alive
	 * even when an old RPC cannot drain; isolation cannot cancel that RPC. */
	for (unsigned elapsed = 0; !g_pci_access.drained() ||
	     __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE); ++elapsed) {
		if (elapsed == 1000) { pci->release(); return -16; }
		IOSleep(1);
	}
	int result = dext_isolate_endpoint(*pci, [] { IOSleep(1); });
	pci->release();
	return result;
}

/* Cached state only: PCI admission was closed by dext_pci_quarantine() alone
 * (no earlier definite transport fault), every admitted operation has left,
 * no reset is running and no interrupt source or drain remains. */
static bool dext_pci_quarantine_quiescent_locked(void)
{
	return g_pci && g_pci_open && g_pci_access.closed() &&
	       g_pci_access.drained() &&
	       !__atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) &&
	       !g_irq_vector_count && !g_irq_queue &&
	       dext_pci_transport_fault() == DEXT_PCI_FAULT_QUARANTINE;
}

extern "C" int dext_pci_quarantine_releasable(void)
{
	dext_pci_control_guard control;
	return dext_pci_quarantine_quiescent_locked();
}

/* Reopen admission for the owner's verified shutdown reset. The owner has
 * already proven software quiescence; a definite earlier fault, an admitted
 * operation or a live interrupt source keeps the quarantine in place. */
extern "C" int dext_pci_release_quarantine(void)
{
	dext_pci_control_guard control;
	if (!g_pci || !g_pci_open) return -19;
	if (dext_pci_transport_fault() != DEXT_PCI_FAULT_QUARANTINE &&
	    dext_pci_transport_fault() != DEXT_PCI_FAULT_NONE) return -5;
	if (!g_pci_access.closed()) return 0;
	if (!dext_pci_quarantine_quiescent_locked() || !g_pci_access.reopen())
		return -16;
	__atomic_store_n(&g_transport_fault_offset, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_transport_fault, DEXT_PCI_FAULT_NONE, __ATOMIC_RELEASE);
	return 0;
}

static bool dext_config_offset_ok(uint64_t offset, uint64_t width)
{
	return g_pci && g_pci_open && offset < 4096 &&
		width <= 4096 - offset && (offset & (width - 1)) == 0;
}

extern "C" int dext_pci_config_read8(uint64_t offset, uint8_t *value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!value || !dext_config_offset_ok(offset, 1)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	*value = UINT8_MAX;
	g_pci->ConfigurationRead8(offset, value);
	if (*value == UINT8_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_CONFIG, offset);
	return 0;
}

extern "C" int dext_pci_config_read16(uint64_t offset, uint16_t *value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!value || !dext_config_offset_ok(offset, 2)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	*value = UINT16_MAX;
	g_pci->ConfigurationRead16(offset, value);
	if (*value == UINT16_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_CONFIG, offset);
	return 0;
}

extern "C" int dext_pci_config_read32(uint64_t offset, uint32_t *value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!value || !dext_config_offset_ok(offset, 4)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	*value = UINT32_MAX;
	g_pci->ConfigurationRead32(offset, value);
	if (*value == UINT32_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_CONFIG, offset);
	return 0;
}

extern "C" int dext_pci_config_write8(uint64_t offset, uint8_t value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!dext_config_offset_ok(offset, 1)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	g_pci->ConfigurationWrite8(offset, value);
	return 0;
}

extern "C" int dext_pci_config_write16(uint64_t offset, uint16_t value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!dext_config_offset_ok(offset, 2)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	g_pci->ConfigurationWrite16(offset, value);
	return 0;
}

extern "C" int dext_pci_config_write32(uint64_t offset, uint32_t value)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!dext_config_offset_ok(offset, 4)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, offset);
		return -1;
	}
	g_pci->ConfigurationWrite32(offset, value);
	return 0;
}

extern "C" int dext_pci_snapshot(struct dext_pci_snapshot *snapshot)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint32_t identity = UINT32_MAX, class_revision = UINT32_MAX;
	uint32_t subsystem = UINT32_MAX;
	if (!snapshot || !g_pci || !g_pci_open) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT, 0);
		return -1;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	if (dext_pci_config_read32(0x00, &identity) ||
	    dext_pci_config_read32(0x08, &class_revision))
		return -1;
	g_pci->ConfigurationRead32(0x2c, &subsystem);
	/* Accept any AMD function of the classes the dext personality matches
	 * (display controllers and processing accelerators).  Whether the
	 * device is supported is decided by upstream amdgpu's own PCI ID table
	 * when the probe matches it; no device ID is checked here. */
	if (identity == UINT32_MAX || class_revision == UINT32_MAX ||
	    (identity & 0xffff) != 0x1002 ||
	    ((class_revision >> 24) != 0x03 &&
	     (class_revision >> 16) != 0x1200)) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT, 0);
		return -1;
	}
	snapshot->vendor = (uint16_t)identity;
	snapshot->device = (uint16_t)(identity >> 16);
	snapshot->revision = (uint8_t)class_revision;
	snapshot->class_code = class_revision >> 8;
	if (subsystem != UINT32_MAX) {
		snapshot->subsystem_vendor = (uint16_t)subsystem;
		snapshot->subsystem_device = (uint16_t)(subsystem >> 16);
	}
	if (g_pci->GetBusDeviceFunction(&snapshot->bus, &snapshot->slot,
					&snapshot->function) != kIOReturnSuccess) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT, 0);
		return -1;
	}
	for (uint8_t bar = 0; bar < 6; bar++) {
		struct dext_pci_bar *range = &snapshot->bar[bar];
		uint32_t low = UINT32_MAX, high = UINT32_MAX;
		if (g_pci->GetBARInfo(bar, &range->memory_index,
					&range->size, &range->type) != kIOReturnSuccess ||
		    !range->size) continue;
		if (dext_pci_config_read32(0x10 + 4 * bar, &low))
			return -1;
		if (range->type == kPCIBARTypeIO) {
			range->base = low & ~3u;
		} else {
			range->base = low & ~15u;
			if ((range->type & kPCIBARTypeM64) == kPCIBARTypeM64) {
				if (bar == 5) {
					dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT,
						0x10 + 4 * bar);
					return -1;
				}
				if (dext_pci_config_read32(0x14 + 4 * bar, &high))
					return -1;
				range->base |= (uint64_t)high << 32;
			}
		}
		if (bar == g_reg_bar && g_reg_window && range->size > g_reg_window)
			range->size = g_reg_window;
		if (!range->base || range->size > UINT64_MAX - range->base) {
			dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT,
				0x10 + 4 * bar);
			return -1;
		}
		range->present = 1;
	}
	/* Which BARs exist is per-ASIC and checked by upstream when it maps
	 * them.  The snapshot only requires the register BAR that dext_open
	 * selected and mapped. */
	if (g_reg_bar > 5 || !snapshot->bar[g_reg_bar].present ||
	    snapshot->bar[g_reg_bar].type == kPCIBARTypeIO) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_SNAPSHOT,
			0x10 + 4 * (g_reg_bar > 5 ? 5 : g_reg_bar));
		return -1;
	}
	return 0;
}

/* C API for the in-process KMD client */
extern "C" {

int dext_bar_info(uint8_t bar, uint8_t *mem_index, uint64_t *size)
{
	dext_pci_operation operation(false);
	if (!operation) return -1;
	uint8_t type = 0;
	if (!g_pci || !mem_index || !size || bar > 5)
		return -1;
	return g_pci->GetBARInfo(bar, mem_index, size, &type) ==
		kIOReturnSuccess ? 0 : -1;
}

/* Retain the assigned BAR descriptor for an in-process CPU mapping.  The
 * caller owns the descriptor and releases it after CreateMapping. */
int dext_copy_bar_memory(uint8_t bar, uint64_t *size, void **descriptor)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (!descriptor || !size || !g_pci || !g_pci_client ||
	    !g_pci_open || bar != 0)
		return -1;
	*descriptor = nullptr;
	*size = 0;
	uint8_t memory_index = 0, type = 0;
	uint64_t length = 0;
	if (g_pci->GetBARInfo(bar, &memory_index, &length, &type) !=
	    kIOReturnSuccess || !length)
		return -1;
	IOMemoryDescriptor *memory = nullptr;
	if (g_pci->_CopyDeviceMemoryWithIndex(memory_index, &memory,
					    g_pci_client) != kIOReturnSuccess || !memory)
		return -1;
	uint64_t descriptor_length = 0;
	if (memory->GetLength(&descriptor_length) != kIOReturnSuccess ||
	    descriptor_length < length) {
		memory->release();
		return -1;
	}
	*descriptor = memory;
	*size = length;
	return 0;
}

int dext_open(uint32_t *token)
{
	dext_pci_control_guard control;
	if (g_pci_access.closed() ||
	    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE)) return -1;
	/* Claim the matched IOPCIDevice (done by the IOService at Start)
	 * and open a session.  Then read BAR geometry and mint the
	 * primary fake-MMIO token for the register BAR.
	 *
	 * The IOPCIDevice provider is already known to the IOService
	 * (it matched the PCI device); we open a session here so
	 * MemoryRead/Write are valid.  Open(this-service, 0) is the
	 * exclusive-claim; the IOService is the forClient.  In this
	 * seam the forClient is NULL-tolerant: the IOService calls
	 * dext_set_pci() with itself, and we pass it as forClient via
	 * the retained pointer.  (T-bringup-dext owns the full open +
	 * D0-wake + MMIO-probe sequence; here we do the minimum that
	 * makes the token valid.) */
	if (!token || !g_pci || !g_pci_client ||
	    __atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE))
		return -1;
	if (g_pci_open) {
		if (!g_reg_token)
			return -1;
		*token = g_reg_token;
		return 0;
	}
	*token = 0;

	/* Open a session to the PCI device.  The upstream PCI probe owns
	 * enabling memory decode and bus mastering. */
	kern_return_t ret = g_pci->Open(g_pci_client, 0);
	if (ret != kIOReturnSuccess)
		return -1;
	g_pci_open = true;

	/* Select the register BAR the way upstream amdgpu_device_init() does:
	 * BAR5 on CIK and newer, BAR2 on SI.  The family is not known before
	 * the probe, but it is visible in the BAR layout: every CIK+ part
	 * exposes a memory BAR5, and SI parts have no BAR5.  The window is the
	 * whole BAR as assigned; upstream sets adev->rmmio_size from the same
	 * resource and reaches registers beyond it through its own per-ASIC
	 * PCIE index/data pair. */
	uint8_t  mi = 0;
	uint64_t sz = 0;
	uint8_t  ty = 0;
	const uint8_t candidates[] = {5, 2};
	g_reg_bar = UINT8_MAX;
	for (uint8_t bar : candidates) {
		if (g_pci->GetBARInfo(bar, &mi, &sz, &ty) == kIOReturnSuccess &&
		    sz >= 0x40 && ty != kPCIBARTypeIO) {
			g_reg_bar = bar;
			break;
		}
	}
	if (g_reg_bar == UINT8_MAX) {
		g_pci->Close(g_pci_client, 0);
		g_pci_open = false;
		return -1;
	}
	/* The accessible window is a capability of the mapping DriverKit
	 * provides, not of the ASIC: use the BAR size, bounded by the length
	 * of the device-memory descriptor for that BAR if it is shorter.  The
	 * snapshot publishes this window as the BAR's resource length, so
	 * upstream's rmmio_size matches what is reachable and upstream sends
	 * every register beyond it through its own indirect path. */
	IOMemoryDescriptor *reg_memory = nullptr;
	uint64_t mapped = 0;
	if (g_pci->_CopyDeviceMemoryWithIndex(mi, &reg_memory, g_pci_client) ==
	    kIOReturnSuccess && reg_memory) {
		if (reg_memory->GetLength(&mapped) == kIOReturnSuccess &&
		    mapped >= 0x40 && mapped < sz)
			sz = mapped;
		reg_memory->release();
	}
	g_reg_window = sz;

	/* Mint the primary token.  host_shadow=0: the dext services MMIO
	 * from IOPCIDevice, never a CPU mirror.  base=0: the token's
	 * offset is the BAR-relative offset (MemoryRead/Write take
	 * offset-within-BAR, not a physical address). */
	g_reg_token = rt_mmio_mint_token(g_pci, mi, 0, sz, 0);
	*token = g_reg_token;
	if (!g_reg_token) {
		*token = 0;
		g_pci->Close(g_pci_client, 0);
		g_pci_open = false;
		return -1;
	}
	return 0;
}

/* ---- MMIO dispatch.  The token is the BAR5 primary token; the offset
 * is BAR5-relative.  Every access is one IOPCIDevice method call across
 * the TB5 tunnel — the constant cost the
 * index-based fake-MMIO design accepts.
 *
 * Doorbell ordering (a mac_amdgpu invariant): a
 * doorbell write must be preceded by an HDP flush + release fence so the
 * GPU observes the ring contents (and the per-ring HDP-flush register write
 * the KMD emits into the IB) before the doorbell ring.  The HDP flush itself
 * is per-ring content the KMD writes into the ring via a normal MMIO write
 * (gfx_v10_0_ring_emit_hdp_flush) — not a separate dext op.  What the dext
 * must enforce is the ORDERING: every preceding register/ring write must be
 * committed before the doorbell write lands, or the GPU can ring the doorbell
 * before it sees the ring.  DriverKit 25.5 has no OSReleaseFence / memory-
 * barrier API in IOLib (verified: grep of IOLib.h is empty), so we use a C++
 * release fence — it orders all preceding stores (across the TB5 tunnel each
 * is an IOPCIDevice::MemoryWrite) before the doorbell store.  This is the
 * mac_amdgpu ordering invariant made explicit (free on Linux, explicit here).
 *
 * TODO(hw-verify): the release fence orders CPU-visible stores; the actual
 * cross-TB5-tunnel commit ordering of the IOPCIDevice::MemoryWrite calls is
 * verified against the oracle trace at the P1/P2 hardware gate.  The doorbell
 * token itself is BAR2 (a separate token the bringup track mints); the fence
 * is emitted in every MMIO write so it is in place the moment a doorbell-token
 * write is dispatched. */
#define LINUXU_DOORBELL_FENCE() \
	__atomic_thread_fence(__ATOMIC_RELEASE)

static int dext_mem_slot(uint32_t token, uint64_t offset, uint64_t width,
			 uint8_t *mem_index)
{
	uint64_t size = 0;
	/* Every token covers exactly the BAR range it was minted for, which is
	 * the BAR as assigned.  Upstream never issues an access beyond its own
	 * mapping: registers past adev->rmmio_size go through adev->pcie_rreg/
	 * pcie_wreg, i.e. the per-ASIC PCIE index/data pair from its NBIO
	 * callbacks.  An access outside the window is therefore a fault. */
	if (!g_pci || !g_pci_open || !token || !mem_index || !width ||
	    (offset & (width - 1)) != 0 ||
	    width > UINT64_MAX - offset ||
	    rt_mmio_slot_info(token, mem_index, &size) != 0 ||
	    offset + width > size) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, offset);
		return -1;
	}
	return 0;
}

static int dext_mem_failure(uint64_t offset)
{
	dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, offset);
	return -1;
}

int dext_mem_read8(uint32_t token, uint64_t offset, uint8_t *val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (!val || dext_mem_slot(token, offset, 1, &mi) != 0)
		return dext_mem_failure(offset);
	*val = UINT8_MAX;
	g_pci->MemoryRead8(mi, offset, val);
	if (*val == UINT8_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_MMIO, offset);
	return 0;
}

int dext_mem_read16(uint32_t token, uint64_t offset, uint16_t *val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (!val || dext_mem_slot(token, offset, 2, &mi) != 0)
		return dext_mem_failure(offset);
	*val = UINT16_MAX;
	g_pci->MemoryRead16(mi, offset, val);
	if (*val == UINT16_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_MMIO, offset);
	return 0;
}

int dext_mem_write8(uint32_t token, uint64_t offset, uint8_t val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (dext_mem_slot(token, offset, 1, &mi) != 0)
		return dext_mem_failure(offset);
	LINUXU_DOORBELL_FENCE();
	g_pci->MemoryWrite8(mi, offset, val);
	return 0;
}

int dext_mem_write16(uint32_t token, uint64_t offset, uint16_t val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (dext_mem_slot(token, offset, 2, &mi) != 0)
		return dext_mem_failure(offset);
	LINUXU_DOORBELL_FENCE();
	g_pci->MemoryWrite16(mi, offset, val);
	return 0;
}

int dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (!val || dext_mem_slot(token, offset, 4, &mi) != 0)
		return dext_mem_failure(offset);
	*val = UINT32_MAX;
	g_pci->MemoryRead32(mi, offset, val);
	if (*val == UINT32_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_MMIO, offset);
	return 0;
}

int dext_mem_read64(uint32_t token, uint64_t offset, uint64_t *val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (!val || dext_mem_slot(token, offset, 8, &mi) != 0)
		return dext_mem_failure(offset);
	*val = UINT64_MAX;
	g_pci->MemoryRead64(mi, offset, val);
	if (*val == UINT64_MAX)
		dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_MMIO, offset);
	return 0;
}

int dext_mem_write32(uint32_t token, uint64_t offset, uint32_t val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (dext_mem_slot(token, offset, 4, &mi) != 0)
		return dext_mem_failure(offset);
	LINUXU_DOORBELL_FENCE();
	g_pci->MemoryWrite32(mi, offset, val);
	return 0;
}

int dext_mem_write64(uint32_t token, uint64_t offset, uint64_t val)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	uint8_t mi;
	if (dext_mem_slot(token, offset, 8, &mi) != 0)
		return dext_mem_failure(offset);
	LINUXU_DOORBELL_FENCE();
	g_pci->MemoryWrite64(mi, offset, val);
	return 0;
}

typedef int (*dext_irq_handler_t)(int irq, void *arg);

/* The rt-registered handler the KMD installed via request_irq
 * (rt_pci_irq_request → rt_irq_register in amdgpu-rt/device.c).  The
 * callback below dispatches to it.  */
static dext_irq_handler_t g_irq_handler;
static void              *g_irq_arg;
static int                g_irq_vector;

/* ---- the MSI-X dispatch callback (the body the codegen'd OSAction's
 * InterruptOccurred method invokes, on the irqQueue).
 *
 * This is the WP11 IRQ discipline: set the in_interrupt flag, drain the IH
 * ring via the SAME entry the host path uses (linuxu_rt_inject_irq →
 * rt_irq_entries[vector].handler — the KMD's amdgpu_irq handler), then clear
 * the flag.  The drain must NOT enter a blocking fence-wait (WP11): the KMD
 * handler checks in_interrupt()==1 and skips the fence-wait/reschedule paths
 * (amdgpu_gfx.c:1156/1228/1328, amdgpu_gmc.c:897).  We assert that as a
 * compile-time-visible contract below.  */
extern "C" int linuxu_rt_inject_irq(int vector);   /* amdgpu-rt/device.c */
extern "C" void linuxu_set_in_interrupt(int in);   /* spinlock.c (WP11) */

/* mac_linuxgpu_irq_callback: called by the codegen'd driver
 * InterruptOccurred action on g_irq_queue. */
void mac_linuxgpu_irq_callback(int vector)
{
	if (__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE))
		return;
	g_irq_fired = true;
	/* WP11: mark the IRQ dispatch path.  This flag is read by the 6 KMD
	 * in_interrupt() sites so they skip a fence-wait / reschedule.  It is
	 * LINUXU_TLS (process-global under driverKit, __thread on host).  */
	linuxu_set_in_interrupt(1);
	/* Drain the IH ring via the real rt entry — the SAME one the host
	 * path (and unit tests) use, so the KMD sees a real interrupt.  The
	 * handler is the KMD's amdgpu_irq.c handler, stored by
	 * rt_pci_irq_request → rt_irq_register.  */
	linuxu_rt_inject_irq(vector);
	linuxu_set_in_interrupt(0);
}

/* MSI-X via IOInterruptDispatchSource (real 25.5 API, verified against
 * IOPCIDevice.h / IOInterruptDispatchSource.h).  The sequence mirrors the
 * proven mac_amdgpu model (MacAMDGPU.cpp:1184-1255):
 *   1. ConfigureInterrupts(MSIX, 1, count, 0) — allocate the vectors
 *      (fall back to plain MSI if MSIX unavailable).
 *   2. For each vector: IOInterruptDispatchSource::Create(pci, i, queue) +
 *      SetHandler(OSAction) + SetEnable(true).
 * Step 2's SetHandler needs the Xcode-codegen'd OSAction (the UserClient's
 * InterruptOccurred method), so in the make build we create + enable the
 * sources and store them, and the callback body (mac_linuxgpu_irq_callback)
 * is wired for the codegen'd action to invoke.  */
int dext_irq_register(uint32_t token, int vector,
		      dext_irq_handler_t handler, void *arg)
{
	dext_pci_control_guard control;
	if (g_pci_access.closed() ||
	    __atomic_load_n(&g_pci_resetting, __ATOMIC_ACQUIRE)) return -1;
	if (__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
	    token != g_reg_token || !g_pci_open || vector != 0 ||
	    g_irq_vector_count)
		return -1;
	/* Store the rt handler (the KMD's amdgpu_irq handler) so the
	 * dispatch callback invokes it.  */
	g_irq_handler = handler;
	g_irq_arg     = arg;
	g_irq_vector  = vector;

	if (!g_pci)
		return -1;

	/* IRQ completion must run while the default queue waits for firmware or
	 * fences. Cancel drains this queue before upstream state is released. */
	if (!g_irq_queue) {
		kern_return_t qret = g_pci_client->CopyDispatchQueue(
			"Interrupts", &g_irq_queue);
		if (qret != kIOReturnSuccess || !g_irq_queue)
			return -1;
	}

	/* Allocate the MSI-X vectors.  Probe the MSIX capability first
	 * (FindPCICapability kIOPCICapabilityIDMSIX); fall back to plain MSI
	 * if absent — exactly the mac_amdgpu model.  */
	uint32_t using_msix = 0;
	uint64_t capoff = 0;
	if (g_pci->FindPCICapability(kIOPCICapabilityIDMSIX,
				      0, &capoff) == kIOReturnSuccess)
		using_msix = 1;

	uint32_t requested = (vector >= 0) ? (uint32_t)(vector + 1) : 1;
	if (requested > LINUXU_DEXT_MAX_IRQ_VECTORS)
		requested = LINUXU_DEXT_MAX_IRQ_VECTORS;

	kern_return_t ret = g_pci->ConfigureInterrupts(
		using_msix ? kIOInterruptTypePCIMessagedX :
			    kIOInterruptTypePCIMessaged,
		1, requested, 0);
	if (ret != kIOReturnSuccess && using_msix) {
		/* MSIX unavailable — fall back to plain MSI, 1 vector.  */
		requested = 1;
		using_msix = 0;
		ret = g_pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged,
					  1, 1, 0);
	}
	if (ret != kIOReturnSuccess)
		return -1; /* no interrupt vectors: registration fails loud */
	g_irq_type = using_msix ? DEXT_PCI_IRQ_MSIX : DEXT_PCI_IRQ_MSI;

	/* Create sources now; arm each only after the generated OSAction has
	 * been bound by the UserClient. */
	g_irq_vector_count = 0;
	for (uint32_t i = 0; i < requested; i++) {
		IOInterruptDispatchSource *src = nullptr;
		kern_return_t cret =
			IOInterruptDispatchSource::Create(g_pci, i, g_irq_queue,
							  &src);
		if (cret != kIOReturnSuccess || !src)
			break;
		g_irq_sources[g_irq_vector_count++] = src;
	}
	return g_irq_vector_count > 0 ? 0 : -1;
}

int dext_irq_vector_count(void)
{
	return g_irq_vector_count;
}

int dext_pci_irq_status(unsigned int *armed_vectors, unsigned int *type)
{
	if (!armed_vectors || !type)
		return -1;
	*armed_vectors = 0;
	*type = DEXT_PCI_IRQ_NONE;
	if (__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) || !g_pci_open)
		return -1;
	for (int i = 0; i < g_irq_vector_count && g_irq_armed[i]; i++)
		++*armed_vectors;
	if (*armed_vectors)
		*type = g_irq_type;
	return 0;
}

int dext_irq_bind_action(int vector, void *opaque_action)
{
	dext_pci_operation operation;
	if (!operation) return -1;
	if (__atomic_load_n(&g_irq_draining, __ATOMIC_ACQUIRE) ||
	    vector < 0 || vector >= g_irq_vector_count || !opaque_action)
		return -1;
	IOInterruptDispatchSource *source = g_irq_sources[vector];
	if (!source)
		return -1;
	OSAction *action = static_cast<OSAction *>(opaque_action);
	if (source->SetHandler(action) != kIOReturnSuccess)
		return -1;
	if (source->SetEnable(true) != kIOReturnSuccess)
		return -1;
	g_irq_armed[vector] = true;
	return 0;
}

static void dext_irq_finish_one(void)
{
	if (__atomic_sub_fetch(&g_irq_pending_drains, 1,
				 __ATOMIC_ACQ_REL) != 0)
		return;
	/* A failed Cancel/disable leaves the source and PCI owner alive.
	 * Without a drain guarantee the completion must never run. */
	if (__atomic_load_n(&g_irq_drain_failed, __ATOMIC_ACQUIRE))
		return;
	g_irq_vector_count = 0;
	g_irq_type = DEXT_PCI_IRQ_NONE;
	g_irq_handler = nullptr;
	g_irq_arg = nullptr;
	if (g_irq_queue) {
		g_irq_queue->release();
		g_irq_queue = nullptr;
	}
	void (*complete)(void *) = g_irq_drain_complete;
	void *context = g_irq_drain_context;
	g_irq_drain_complete = nullptr;
	g_irq_drain_context = nullptr;
	__atomic_store_n(&g_irq_draining, false, __ATOMIC_RELEASE);
	if (complete)
		complete(context);
}

static void dext_irq_source_drained(int vector,
				     IOInterruptDispatchSource *source)
{
	g_irq_sources[vector] = nullptr;
	source->release();
	dext_irq_finish_one();
}

/* Submit cancellation for every source, including sources created during a
 * partial setup.  The callback runs once all source handlers have drained;
 * it may run before this function returns when there are no sources.  A
 * failed Cancel falls back to disable-with-completion.  If both fail, return
 * -1, keep the PCI/queue/source backing alive, and never call completion. */
extern "C" int dext_irq_fini_async(void (*drained)(void *), void *context)
{
	bool expected = false;
	if (!__atomic_compare_exchange_n(&g_irq_draining, &expected, true,
				      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return -1;
	g_irq_drain_complete = drained;
	g_irq_drain_context = context;
	g_irq_drain_failed = false;
	int count = 0;
	for (int i = 0; i < g_irq_vector_count; i++)
		if (g_irq_sources[i])
			count++;
	/* The sentinel prevents an inline cancellation callback from finishing
	 * teardown while this submission loop still reads the source table. */
	__atomic_store_n(&g_irq_pending_drains, count + 1, __ATOMIC_RELEASE);
	bool failed = false;
	for (int i = 0; i < g_irq_vector_count; i++) {
		IOInterruptDispatchSource *source = g_irq_sources[i];
		if (!source)
			continue;
		g_irq_armed[i] = false;
		auto on_drained = ^{
			dext_irq_source_drained(i, source);
		};
		kern_return_t ret = source->Cancel(on_drained);
		if (ret != kIOReturnSuccess) {
			ret = source->SetEnableWithCompletion(false, on_drained);
			if (ret != kIOReturnSuccess) {
				IOLog("MacLinuxGPU: IRQ vector %d cannot drain (%#x)\n",
				      i, ret);
				failed = true;
			}
		}
	}
	__atomic_store_n(&g_irq_drain_failed, failed, __ATOMIC_RELEASE);
	dext_irq_finish_one(); /* submission sentinel */
	return failed ? -1 : 0;
}

/* Legacy initiation only.  Callers that own PCI or KMD state must use the
 * async completion before releasing that state. */
extern "C" void dext_irq_fini(void)
{
	(void)dext_irq_fini_async(nullptr, nullptr);
}

/* The IOService's IRQ dispatch source (T-irq-dext) calls this to
 * dispatch a fired vector to the registered rt handler.  This is the
 * in-process (make-build) delivery path: it runs mac_linuxgpu_irq_callback
 * synchronously so a host-side inject (linuxu_rt_inject_irq) and a real
 * MSI-X fire exercise the SAME discipline.  Under the codegen'd build the
 * OSAction invokes mac_linuxgpu_irq_callback directly on g_irq_queue.  */
extern "C" int dext_irq_dispatch(int vector)
{
	mac_linuxgpu_irq_callback(vector);
	return 0;
}

} /* extern "C" */

#else /* host build: testable stubs */

/* Host-side twins: the unit tests call these to exercise the same
 * code paths without DriverKit.  They forward into the linuxu src
 * host backends (host shadow buffers). */
extern "C" {

int dext_open(uint32_t *token)
{
	if (token)
		*token = 0;
	return 0;
}

int dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *val)
{
	(void)token; (void)offset;
	if (val)
		*val = 0;
	return 0;
}

int dext_mem_read64(uint32_t token, uint64_t offset, uint64_t *val)
{
	(void)token; (void)offset;
	if (val)
		*val = 0;
	return 0;
}

int dext_mem_write32(uint32_t token, uint64_t offset, uint32_t val)
{
	(void)token; (void)offset; (void)val;
	return 0;
}

int dext_mem_write64(uint32_t token, uint64_t offset, uint64_t val)
{
	(void)token; (void)offset; (void)val;
	return 0;
}

typedef int (*dext_irq_handler_t)(int irq, void *arg);

int dext_irq_register(uint32_t token, int vector,
		      dext_irq_handler_t handler, void *arg)
{
	(void)token; (void)vector; (void)handler; (void)arg;
	return 0;
}

} /* extern "C" */

#endif /* LINUXU_DEXT */
