//
//  MacLinuxGPUXcode.mm — the Xcode DriverKit build's OSMetaClass
//  registration + method bodies + the UserClient ExternalMethod dispatch.
//
//  THIS FILE IS THE XCODE-BUILD-ONLY DEXT CLASS REGISTRATION.  It is the
//  "T-hostapp-sysextd" deliverable: the `iig` codegen (run by the Xcode
//  DriverKit build-phase on MacLinuxGPU.iig / MacLinuxGPUUserClient.iig)
//  produces the derived class headers (MacLinuxGPU.h /
//  MacLinuxGPUUserClient.h) + the sMetaClass / OSClassDescription method
//  tables; this .mm provides the method BODIES that the codegen'd
//  OSClassDescription dispatches to.  The make build does NOT compile
//  this file (it has no iig codegen); it uses the hand-rolled
//  MacLinuxGPU.mm (the lifecycle as free functions) instead.
//
//  Architecture:
//    - MacLinuxGPU::Start/Stop/NewUserClient/free — the IOService
//      lifecycle. Start registers the provider without opening PCI.
//      Client operations own PCI sessions; InitDevice allocates the Linux
//      device and invokes the upstream AMDGPU PCI probe. The
//      upstream driver owns allocation and initialization of its adev.
//    - MacLinuxGPUUserClient::ExternalMethod — the selector-RPC dispatch.
//      The kMacAMDGPUMethod* numbers are kept IDENTICAL to
//      mac_amdgpu/MacAMDGPU.cpp so the host app (the ported
//      MacLinuxGPUHostApp) reuses its client ABI.  Each selector routes to
//      the dext_* C API in dext_main.m / iokit_bridge.m (the same seam the
//      make build's C API uses).
//
//  This file is the ONE thing the make build stubs: the real OSMetaClass
//  registration that makes IOKit instantiate MacLinuxGPU on PCI match +
//  spawn the UserClient per open (the make build's NewUserClient returns
//  kIOReturnUnsupported; this one returns the codegen'd
//  MacLinuxGPUUserClient).
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IOUserClient.iig>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSString.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

// The codegen'd class headers (the iig tool generates MacLinuxGPU.h +
// MacLinuxGPUUserClient.h from the .iig files at Xcode build time).
#include "MacLinuxGPU.h"
#include "MacLinuxGPUUserClient.h"

// The compute selector seam (T-dext-userclient-seam): the C API the COMPUTE
// selectors (RuntimeBuild/QueryInfo/BO*/AQL*/HostWindow/Shutdown/GetReBAR/
// HostMemoryTest/WaitFence/SubmitIB/CS*) route to.  Implemented in
// dext_compute.c (pure C, host-testable); the DriverKit I/O is the optional
// gpu-op hook (dext_compute_dk.mm, dext-only).
#include "dext_compute.h"
#include "observer_gate.h"
#include "raw_bar_lease.h"
#include "session_state.h"
#include <rt/bootstrap.h>
#include <rt/drm_info.h>
#include <rt/sysfs.h>
#include <rt/dext_pci.h>
#include <rt/dext_dma.h>
#include <rt/gart.h>
#include <rt/klog.h>
#include <rt/fw_mailbox.h>

extern "C" uint64_t linuxu_dart_budget(void);

// The ABI headers the compute selectors carry (copied from mac_amdgpu,
// self-contained).  The AQLDispatch / ComputeDispatch struct layouts.
#include "../amdgpu/amdgpu_aql_abi.h"
#include "../amdgpu/amdgpu_dispatch_abi.h"

// ----------------------------------------------------------------
// The selector-RPC contract (the host app <-> dext ABI).
//
// The kMacAMDGPUMethod* numbers are kept IDENTICAL to
// mac_amdgpu/MacAMDGPU.cpp (lines 61-92) so the ported host app
// (MacLinuxGPUHostApp) reuses its client ABI.  The host app speaks these
// numbers to the UserClient; the UserClient (this file) dispatches them to
// the dext_* C API.
// ----------------------------------------------------------------
enum {
    kMacAMDGPUMethodPing              = 0,
    kMacAMDGPUMethodGetIdentity       = 1,
    kMacAMDGPUMethodGetBARInfo        = 2,
    kMacAMDGPUMethodSetupInterrupts   = 3,
    kMacAMDGPUMethodWaitInterrupt     = 4,   // async
    kMacAMDGPUMethodSetIRQMask        = 5,
    kMacAMDGPUMethodAllocateDMABuffer = 6,
    kMacAMDGPUMethodFreeDMABuffer     = 7,
    kMacAMDGPUMethodResetDevice       = 8,
    kMacAMDGPUMethodInitDevice        = 9,
    kMacAMDGPUMethodLoadFirmware      = 10,
    kMacAMDGPUMethodSetIPBase         = 11,
    kMacAMDGPUMethodGetIPBase         = 12,
    kMacAMDGPUMethodLoadDiscoveryBin  = 13,
    kMacAMDGPUMethodSubmitTestPM4     = 14,
    kMacAMDGPUMethodSDMACopyTest      = 15,
    kMacAMDGPUMethodBOAlloc           = 16,
    kMacAMDGPUMethodBOFree            = 17,
    kMacAMDGPUMethodBOGetInfo         = 18,
    kMacAMDGPUMethodSubmitIB          = 19,
    kMacAMDGPUMethodWaitFence         = 20,
    kMacAMDGPUMethodQueryInfo         = 21,
    kMacAMDGPUMethodMESAddQueue       = 22,
    kMacAMDGPUMethodGetDiagnostics    = 23,
    kMacAMDGPUMethodDumpTMR           = 24,
    kMacAMDGPUMethodDumpPSP           = 25,
    kMacAMDGPUMethodDumpCmdBuf        = 26,
    kMacAMDGPUMethodLiveStatus        = 30,
    kMacAMDGPUMethodDisableSmuFeatures = 33,
    kMacAMDGPUMethodBOMap             = 36,
    // v0.1.28 — command-stream submission ABI (SDMA only; GFX/compute
    // unsupported in the current seam).
    kMacAMDGPUMethodCSCreate          = 37,
    kMacAMDGPUMethodCSWriteDwords     = 38,
    kMacAMDGPUMethodCSDestroy         = 39,
    // v0.1.29 — per-state GFXCLK soft-clamp (scalarInput[0] selects the state).
    kMacAMDGPUMethodSetPowerState      = 40,
    kMacAMDGPUMethodGetReBARInfo       = 41, // read-only PCIe capability query
    kMacAMDGPUMethodShutdownGPU        = 42, // reset, close PCI, discard session
    kMacAMDGPUMethodRuntimeBuild       = 43, // actual responding binary, no HW access
    kMacAMDGPUMethodHostMemoryTest     = 44, // data-verified SDMA transfers via GART
    kMacAMDGPUMethodBOCopy            = 48,
    kMacAMDGPUMethodBOWrite           = 49,
    kMacAMDGPUMethodBORead            = 50,
    kMacAMDGPUMethodComputeDispatch    = 51, // owned code BO + launch parameters
    kMacAMDGPUMethodBOExport           = 52,
    kMacAMDGPUMethodBOImport           = 53,
    kMacAMDGPUMethodHostWindow         = 54, // establish/query CPU/GPU GART range
    kMacAMDGPUMethodAQLDispatch        = 55, // bounded owned AQL queue dispatch
    kMacAMDGPUMethodAQLQueueCreate    = 56,
    kMacAMDGPUMethodAQLQueueKick      = 57,
    kMacAMDGPUMethodAQLQueueDestroy   = 58,
    kMacAMDGPUMethodAQLQueueService   = 59,
    kMacAMDGPUMethodAtomicRequester   = 60, // PCIe AtomicOp requester status
    kMacAMDGPUMethodReleaseQuarantine = 61, // entitled: release a quiescent quarantine
};
static_assert(kMacAMDGPUMethodReleaseQuarantine == MLG_SELECTOR_RELEASE_QUARANTINE &&
              kMacAMDGPUMethodQueryInfo == MLG_SELECTOR_QUERY_INFO &&
              kMacAMDGPUMethodRuntimeBuild == MLG_SELECTOR_RUNTIME_BUILD &&
              kMacAMDGPUMethodPing == MLG_SELECTOR_PING, "observer selector numbers");
static_assert(MLG_SELECTOR_SYSFS_READ > kMacAMDGPUMethodReleaseQuarantine &&
              MLG_SELECTOR_DRM_INFO > kMacAMDGPUMethodReleaseQuarantine, "Linux read selectors");
static_assert(DEXT_COMPUTE_QUERY_SESSION_STATE == MLG_QUERY_SESSION_STATE &&
              DEXT_COMPUTE_QUERY_PROBE_STATUS == MLG_QUERY_PROBE_STATUS &&
              DEXT_COMPUTE_QUERY_KERNEL_LOG == MLG_QUERY_KERNEL_LOG, "observer query tags");

// ----------------------------------------------------------------
// The dext_* C API seam (dext_main.m + iokit_bridge.m, LINUXU_DEXT branch).
// The IOService hands the claimed IOPCIDevice to the seam; the seam owns
// dext_open / dext_mem_* / dext_irq_* / dext_dma_*.
// ----------------------------------------------------------------
extern "C" int  dext_set_pci(void *pci_device, void *client);
extern "C" void dext_close(void);
extern "C" void *dext_dma_copy_descriptor(void *cpu_addr);
extern "C" void *dext_dma_copy_ranges_descriptor(const uint64_t *addresses,
                                                const uint64_t *lengths, size_t count);
extern "C" void dext_irq_fini(void);
extern "C" int  dext_irq_fini_async(void (*drained)(void *), void *context);
extern "C" int  dext_open(uint32_t *token);
extern "C" int  dext_mem_read32(uint32_t token, uint64_t offset, uint32_t *val);
extern "C" int  dext_mem_read64(uint32_t token, uint64_t offset, uint64_t *val);
extern "C" int  dext_mem_write32(uint32_t token, uint64_t offset, uint32_t val);
extern "C" int  dext_mem_write64(uint32_t token, uint64_t offset, uint64_t val);
extern "C" int  dext_irq_register(uint32_t token, int vector,
                                  int (*handler)(int, void *), void *arg);
extern "C" int  dext_irq_vector_count(void);
extern "C" int  dext_irq_bind_action(int vector, void *action);
extern "C" int  dext_irq_dispatch(int vector);
extern "C" int  dext_dma_set_pci(void *pci_device);
extern "C" int  dext_dma_fini(void);
extern "C" int  dext_dma_alloc_coherent(size_t size, void **cpu_addr, uint64_t *iova);
extern "C" int  dext_dma_free_coherent(void *cpu_addr, size_t size);

// Firmware.  The dext cannot open files, so request_firmware() is served by
// (1) blobs a client pushed over LoadFirmware (fw_table_override), (2) the
// on-demand mailbox a client maps through CopyClientMemoryForType with
// MLG_FW_MAILBOX_MEMORY_TYPE and services from the installed firmware
// directory, and (3) the optional embedded table.  Missing names fail with
// -ENOENT, as on Linux.
extern "C" int fw_table_override(const char *name, const void *bytes, size_t size);
extern "C" int fw_mailbox_attach(void *memory, size_t size);
extern "C" void fw_mailbox_detach(void);

// linuxu rt device (build-dk/libmacamgdu-dk.a): alloc/free the in-process
// "kernel" device the KMD runs on.
extern "C" void *rt_device_alloc(void);
extern "C" void  rt_device_free(void *dev);
extern "C" void *rt_device_get_pdev(void *rtdev);
extern "C" struct kobject *rt_device_kobject(void *rtdev);
extern "C" int   rt_pci_probe_result(void *pdev);
extern "C" int   rt_pci_probe_cleanup_retained(void *pdev);
extern "C" int   fw_table_register_embedded(void);

#include "retained_log.h"
static void maclinuxgpu_log_sink(const char *text)
{
    os_log(OS_LOG_DEFAULT, "%{public}s", text);
}
#define MACLINUXGPU_LOG(...) \
    maclinuxgpu::RetainedLog(maclinuxgpu_log_sink, __VA_ARGS__)

// ----------------------------------------------------------------
// MacLinuxGPU (IOService) — the OSMetaClass method bodies.
//
// The codegen'd OSClassDescription (from MacLinuxGPU.iig) dispatches the
// IOKit lifecycle to these.  The logic mirrors the make build's
// mac_linuxgpu_driver_start/stop/new_user_client/free (MacLinuxGPU.mm) but
// as OSMetaClass methods (the codegen'd class).
// ----------------------------------------------------------------

// Shared driver state (owned by the IOService, not per-client).  The
// serial bringup queue (the one-queue model) + the
// retained PCI + the rt device + the adev.
static IODispatchQueue *s_bringupQueue = nullptr;
static IOPCIDevice     *s_retainedPCI  = nullptr;
static void            *s_rtDevice     = nullptr;
static bool             s_modulesRunning = false;
static bool             s_probeAttempted = false;
static int              s_probeResult = 0;
static uint32_t         s_token        = 0;
static bool             s_irqReady     = false;
static bool             s_irqDeliver   = false;
static bool             s_dmaQuarantined = false;
static bool             s_dmaShutdownPrepared = false;
static bool             s_quarantineRetained = false;
static MacLinuxGPU     *s_driver = nullptr;
static bool             s_pciOpen = false;
static bool             s_stopping = false;
static bool             s_sessionClosing = false;
static uint64_t         s_sessionGeneration = 1;
static uint32_t         s_participants = 0;
static uint64_t         s_nextClientID = 0;
static maclinuxgpu::RawBARLease s_rawBARLease;
static IOService       *s_stopProvider = nullptr;
static MacLinuxGPUUserClient *s_stoppingClients = nullptr;
// Cached diagnosis, readable by observer clients while closing/quarantined.
static uint32_t         s_quarantineCause = MLG_QUARANTINE_NONE;
static int              s_quarantineCode = 0;
static uint32_t         s_quarantineObserved = MLG_QUARANTINE_NONE;
static int              s_pciIsolation = 0;
static bool             s_pciIsolationAttempted = false;
static bool             s_finalCleanup = false;
static bool             s_irqDrainFailed = false;
static bool             s_releaseFailed = false;
static bool             s_creatingObserver = false;
// Observer reads (SysfsRead, DrmInfo) run upstream callbacks on observer
// queues. Admitted only while the upstream driver runs in an open session;
// every session close and quarantine closes admission and waits first.
static mlg_observer_gate s_observerReads;
static struct rt_drm_info *s_observerDrm = nullptr; // render file for DrmInfo
static void observer_reads_close()
{
    s_observerReads.close();
    while (!s_observerReads.drained()) IOSleep(1);
}
struct MacLinuxGPUUserClient_IVars {
    MacLinuxGPU *ownerDriver;
    IOService *stopProvider;
    MacLinuxGPUUserClient *nextStopping;
    uint64_t sessionGeneration;
    uint64_t clientID;
    bool stopping;
    bool observer; // read-only: never joins, opens or closes a session
    bool identityRecorded; // pid/name handed to the compute backend
    // Observers run on their own queue and hop to the owner's queue for
    // everything but the Linux reads.
    IODispatchQueue *ownerQueue;
    bool onOwnerQueue;
};

class ComputeClientScope {
    uint64_t previous;
public:
    explicit ComputeClientScope(uint64_t client)
        : previous(dext_compute_select_client(client)) {}
    ~ComputeClientScope() { dext_compute_select_client(previous); }
    ComputeClientScope(const ComputeClientScope &) = delete;
    ComputeClientScope &operator=(const ComputeClientScope &) = delete;
};

// The opening process's pid and name for the client's KFD process. The
// kernel records them on the user client as IOUserClientCreator
// ("pid N, name") once IOServiceOpen returns; without it the KFD process
// gets a counter pid.
static void record_client_identity(MacLinuxGPUUserClient *client)
{
    if (!client->ivars || client->ivars->identityRecorded) return;
    client->ivars->identityRecorded = true;
    int pid = 0;
    char name[32] = {};
    OSDictionary *properties = nullptr;
    if (client->CopyProperties(&properties) == kIOReturnSuccess && properties) {
        OSString *creator = OSDynamicCast(OSString, properties->getObject("IOUserClientCreator"));
        const char *text = creator ? creator->getCStringNoCopy() : nullptr;
        if (text && !strncmp(text, "pid ", 4)) {
            const char *p = text + 4;
            while (*p >= '0' && *p <= '9' && pid < 100000000) pid = pid * 10 + (*p++ - '0');
            if (*p == ',') {
                ++p;
                while (*p == ' ') ++p;
                size_t n = 0;
                while (p[n] && n + 1 < sizeof(name)) { name[n] = p[n]; ++n; }
            } else {
                pid = 0;
            }
        }
        properties->release();
    }
    (void)dext_compute_client_identity(client->ivars->clientID, pid, name[0] ? name : nullptr);
    MACLINUXGPU_LOG("client %llu identity: pid %d name %s", client->ivars->clientID, pid,
                    name[0] ? name : "(unknown)");
}

// Ranges of a BO whose pages are not one allocation (KFD GTT BOs).
struct BOMemoryRanges {
    uint64_t *addresses, *lengths;
    size_t count, capacity;
};
static int collect_bo_range(void *arg, void *cpu, uint64_t bytes)
{
    auto *ranges = static_cast<BOMemoryRanges *>(arg);
    if (ranges->count == ranges->capacity) return -1;
    ranges->addresses[ranges->count] = (uint64_t)(uintptr_t)cpu;
    ranges->lengths[ranges->count] = bytes;
    ++ranges->count;
    return 0;
}
static IOMemoryDescriptor *copy_bo_ranges_descriptor(uint32_t memoryType, uint64_t size)
{
    BOMemoryRanges ranges{};
    ranges.capacity = (size_t)(size / 16384) + 2;
    ranges.addresses = static_cast<uint64_t *>(IOMalloc(ranges.capacity * sizeof(uint64_t)));
    ranges.lengths = static_cast<uint64_t *>(IOMalloc(ranges.capacity * sizeof(uint64_t)));
    IOMemoryDescriptor *descriptor = nullptr;
    if (ranges.addresses && ranges.lengths &&
        dext_compute_bo_memory_ranges(memoryType, collect_bo_range, &ranges) == 0 && ranges.count)
        descriptor = static_cast<IOMemoryDescriptor *>(
            dext_dma_copy_ranges_descriptor(ranges.addresses, ranges.lengths, ranges.count));
    if (ranges.addresses) IOFree(ranges.addresses, ranges.capacity * sizeof(uint64_t));
    if (ranges.lengths) IOFree(ranges.lengths, ranges.capacity * sizeof(uint64_t));
    return descriptor;
}

// All session transitions run on the shared default queue. Cancellation
// completion may run elsewhere, so always enqueue final cleanup here.
static void session_irq_drained(void *context)
{
    auto *driver = static_cast<MacLinuxGPU *>(context);
    MACLINUXGPU_LOG("session close: interrupt drain completed; scheduling final cleanup");
    s_bringupQueue->DispatchAsync(^{
        driver->FinishSession();
        driver->release();
    });
}

// The first trigger names the quarantine. A definite transport fault the PCI
// seam recorded outranks the later step that merely observed its effects.
static void note_quarantine(uint32_t cause, int code)
{
    if (s_quarantineCause != MLG_QUARANTINE_NONE) return;
    const int fault = dext_pci_transport_fault();
    if (fault != DEXT_PCI_FAULT_NONE && fault != DEXT_PCI_FAULT_QUARANTINE) {
        s_quarantineCause = MLG_QUARANTINE_PCI_FAULT;
        s_quarantineCode = fault;
        s_quarantineObserved = cause;
    } else if (cause != MLG_QUARANTINE_NONE) {
        s_quarantineCause = cause;
        s_quarantineCode = code;
    } else {
        return;
    }
    MACLINUXGPU_LOG("quarantine cause: step %u code %d (observed by step %u)",
        s_quarantineCause, s_quarantineCode, s_quarantineObserved);
}

static void quarantine_session(MacLinuxGPU *driver)
{
    observer_reads_close();
    note_quarantine(MLG_QUARANTINE_NONE, 0);
    s_dmaQuarantined = true;
    s_sessionClosing = true;
    if (!s_quarantineRetained) {
        driver->retain();
        s_quarantineRetained = true;
    }
    // Uncertain workers may still reference their device, queues and backing.
    // Keep those owners alive and reject new transport allocations.
    dext_dma_quarantine();
    const int isolated = dext_pci_quarantine();
    s_pciIsolation = isolated;
    s_pciIsolationAttempted = true;
    if (isolated != 0)
        MACLINUXGPU_LOG("quarantine: PCI isolation unconfirmed (%d); retaining all owners", isolated);
}

// Why a quarantined session cannot be released yet, from cached state only.
// Release needs: interrupt drain completed, upstream driver and runtime device
// removed, no compute context or uncertain GPU work, no raw BAR mapping or
// session client, only retired DMA descriptors, and PCI admission closed by
// the quarantine itself with nothing admitted.
static uint32_t release_blocker()
{
    if (!s_dmaQuarantined) return MLG_RELEASE_NOT_QUARANTINED;
    if (s_releaseFailed) return MLG_RELEASE_RESET_FAILED;
    const int fault = dext_pci_transport_fault();
    if (fault != DEXT_PCI_FAULT_NONE && fault != DEXT_PCI_FAULT_QUARANTINE)
        return MLG_RELEASE_PCI_FAULT;
    if (s_irqDrainFailed) return MLG_RELEASE_IRQ_FAILED;
    if (!s_finalCleanup) return MLG_RELEASE_IRQ_PENDING;
    if (s_modulesRunning || s_rtDevice) return MLG_RELEASE_UPSTREAM_RETAINED;
    if (!dext_compute_quiescent()) return MLG_RELEASE_COMPUTE_RETAINED;
    if (s_rawBARLease.hasMappings()) return MLG_RELEASE_RAW_BAR_MAPPED;
    if (s_participants) return MLG_RELEASE_PARTICIPANTS;
    if (!dext_dma_quarantine_releasable()) return MLG_RELEASE_DMA_OWNED;
    if (s_pciOpen && !dext_pci_quarantine_releasable()) return MLG_RELEASE_PCI_BUSY;
    return MLG_RELEASE_READY;
}

static void session_state(uint64_t out[MLG_SESSION_STATE_WORDS])
{
    const uint32_t blocker = release_blocker();
    uint64_t flags = 0;
    if (s_sessionClosing) flags |= MLG_SESSION_FLAG_CLOSING;
    if (s_dmaQuarantined) flags |= MLG_SESSION_FLAG_QUARANTINED;
    if (s_stopping) flags |= MLG_SESSION_FLAG_STOPPING;
    if (s_pciOpen) flags |= MLG_SESSION_FLAG_PCI_OPEN;
    if (s_modulesRunning) flags |= MLG_SESSION_FLAG_MODULES_RUNNING;
    if (s_finalCleanup) flags |= MLG_SESSION_FLAG_FINAL_CLEANUP;
    if (blocker == MLG_RELEASE_READY) flags |= MLG_SESSION_FLAG_RELEASABLE;
    if (s_dmaQuarantined && mlg_release_blocker_permanent(blocker))
        flags |= MLG_SESSION_FLAG_RESTART_REQUIRED;
    if (s_rawBARLease.hasMappings()) flags |= MLG_SESSION_FLAG_RAW_BAR_MAPPED;
    if (s_rtDevice) flags |= MLG_SESSION_FLAG_RUNTIME_DEVICE;
    if (s_pciIsolationAttempted) flags |= MLG_SESSION_FLAG_ISOLATION_ATTEMPTED;
    out[0] = MLG_SESSION_STATE_VERSION;
    out[1] = flags;
    out[2] = s_quarantineCause;
    out[3] = (uint64_t)(int64_t)s_quarantineCode;
    out[4] = s_quarantineObserved;
    out[5] = (uint64_t)(int64_t)s_pciIsolation;
    out[6] = blocker;
    out[7] = s_sessionGeneration;
    out[8] = s_participants;
}

// Shared tail once the endpoint was reset with bus mastering off and every
// DMA descriptor released: close the provider normally and finish Stops.
static void complete_session_close(MacLinuxGPU *driver)
{
    dext_close();
    rt_gart_reset();
    s_dmaShutdownPrepared = false;
    s_pciOpen = false;
    s_token = 0;
    s_participants = 0;
    ++s_sessionGeneration;
    s_sessionClosing = false;
    if (s_quarantineRetained) {
        // Pending Stop and client retains keep the service alive below.
        s_quarantineRetained = false;
        driver->release();
    }
    while (s_stoppingClients) {
        auto *client = s_stoppingClients;
        s_stoppingClients = client->ivars->nextStopping;
        client->FinishStop(client->ivars->stopProvider);
    }
    if (s_stopProvider) {
        IOService *provider = s_stopProvider;
        s_stopProvider = nullptr;
        driver->FinishStop(provider);
    }
}

// Release a quarantined session without process death. Only cached state
// decides; when it proves quiescence, the quarantine rejoins the normal
// shutdown path: reopen PCI admission, reset the endpoint with bus mastering
// off, release retired DMA descriptors, then close the provider. A failure
// re-quarantines and leaves a restart as the only recovery.
static uint32_t release_quarantine(MacLinuxGPU *driver)
{
    const uint32_t blocker = release_blocker();
    if (blocker != MLG_RELEASE_READY) {
        MACLINUXGPU_LOG("quarantine release refused: blocker %u%s", blocker,
            mlg_release_blocker_permanent(blocker) ?
                "; restart required, do not kill the driver" : "; retry after it clears");
        return blocker;
    }
    MACLINUXGPU_LOG("quarantine release: quiescent (cause %u code %d); resetting endpoint",
        s_quarantineCause, s_quarantineCode);
    if (dext_dma_lift_quarantine(s_pciOpen) != 0) return MLG_RELEASE_DMA_OWNED;
    int result = 0;
    if (s_pciOpen) {
        const int aliases = dext_bar0_cpu_release_orphaned();
        if (aliases > 0)
            MACLINUXGPU_LOG("quarantine release: released %d orphaned BAR0 CPU mapping reference(s)", aliases);
        result = aliases < 0 ? aliases : dext_pci_release_quarantine();
        if (!result) result = dext_pci_shutdown_reset();
    }
    if (!result) result = dext_dma_fini();
    if (result) {
        s_releaseFailed = true;
        quarantine_session(driver);
        MACLINUXGPU_LOG("quarantine release failed (%d); restart required, do not kill the driver", result);
        return MLG_RELEASE_RESET_FAILED;
    }
    s_dmaQuarantined = false;
    MACLINUXGPU_LOG("quarantine released after endpoint reset; provider closed");
    complete_session_close(driver);
    return MLG_RELEASE_READY;
}

static void close_session(MacLinuxGPU *driver)
{
    if (s_sessionClosing) return;
    s_sessionClosing = true;
    s_finalCleanup = false;
    // No observer read may run an upstream callback past this point.
    observer_reads_close();
    MACLINUXGPU_LOG("session close begin: probe=%d result=%d modules=%d pci=%d quarantine=%d participants=%u",
        s_probeAttempted, s_probeResult, s_modulesRunning, s_pciOpen,
        s_dmaQuarantined, s_participants);
    // Stop is also reached through forced service termination. It provides
    // no proof that a client's raw BAR mappings have been revoked yet.
    if (s_rawBARLease.hasMappings()) {
        s_dmaQuarantined = true;
        note_quarantine(MLG_QUARANTINE_RAW_BAR_MAPPING, 0);
        MACLINUXGPU_LOG("session close: raw BAR mapping lifetime uncertain; retaining backing");
    }
    if (s_pciOpen && !s_dmaQuarantined) {
        const int held = dext_dma_begin_shutdown(linuxu_dart_budget());
        if (held != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_SHUTDOWN_HOLD, held);
            MACLINUXGPU_LOG("session close: cannot reserve DMA backing for shutdown (%d)", held);
        } else {
            s_dmaShutdownPrepared = true;
        }
    }
    // MES removal and upstream shutdown can wait for fences and interrupts.
    // Their released DMA mappings stay pinned until the post-drain reset.
    if (!s_dmaQuarantined) {
        const int stopped = dext_compute_stop();
        if (stopped != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_COMPUTE_UNCERTAIN, stopped);
            MACLINUXGPU_LOG("session close: GPU completion uncertain (%d); retaining runtime", stopped);
        }
    }
    if (s_modulesRunning && !s_dmaQuarantined) {
        // The observers' render file closes like any client's, first.
        if (auto *drm = __atomic_exchange_n(&s_observerDrm, nullptr, __ATOMIC_ACQ_REL))
            rt_drm_info_close(drm);
        MACLINUXGPU_LOG("session close: removing upstream driver");
        linuxu_driver_shutdown();
        s_modulesRunning = false;
        MACLINUXGPU_LOG("session close: upstream removal completed");
    }
    if (s_dmaQuarantined) quarantine_session(driver);
    __atomic_store_n(&s_irqDeliver, false, __ATOMIC_RELEASE);
    s_irqReady = false;
    driver->retain();
    MACLINUXGPU_LOG("session close: requesting interrupt drain");
    const int drained = dext_irq_fini_async(session_irq_drained, driver);
    if (drained != 0) {
        // No completion means that a source still owns live callbacks.
        // Preserve its provider and backing, as in the reference driver.
        s_irqDrainFailed = true;
        note_quarantine(MLG_QUARANTINE_IRQ_CANCEL, drained);
        quarantine_session(driver);
        MACLINUXGPU_LOG("session close blocked: interrupt cancellation failed (%d)", drained);
    }
}

static kern_return_t ensure_open(MacLinuxGPUUserClient *client)
{
    if (!client->ivars || !client->ivars->ownerDriver || s_stopping ||
        client->ivars->stopping || !s_retainedPCI)
        return kIOReturnNotAttached;
    if (client->ivars->observer) return kIOReturnNotPermitted;
    if (s_sessionClosing || s_dmaQuarantined) return kIOReturnNotReady;
    if (!s_rawBARLease.allowsJoin(client->ivars->clientID)) return kIOReturnBusy;
    if (client->ivars->sessionGeneration == s_sessionGeneration)
        return kIOReturnSuccess;
    // Join only a fully initialized shared session. Backend BO/queue handles
    // remain client-owned even though the reference uses a shared VMID0.
    if (s_participants) {
        uint64_t stage = 0;
        if (dext_compute_query_info(4, &stage, 1) != 1 || stage != 2)
            return kIOReturnBusy;
    }
    if (s_participants == UINT32_MAX) return kIOReturnNoResources;
    if (!s_pciOpen) {
        if (dext_set_pci(s_retainedPCI, client->ivars->ownerDriver) != 0)
            return kIOReturnNotReady;
        if (dext_open(&s_token) != 0) {
            (void)dext_dma_fini();
            dext_close();
            return kIOReturnNotOpen;
        }
        s_pciOpen = true;
        dext_compute_set_pci_open(true);
        // A new session starts with no diagnosis from the previous one.
        s_quarantineCause = s_quarantineObserved = MLG_QUARANTINE_NONE;
        s_quarantineCode = s_pciIsolation = 0;
        s_pciIsolationAttempted = s_irqDrainFailed = s_releaseFailed = false;
    }
    client->ivars->sessionGeneration = s_sessionGeneration;
    ++s_participants;
    return kIOReturnSuccess;
}

static kern_return_t prepare_interrupts(MacLinuxGPU *driver)
{
    if (s_irqReady) return kIOReturnSuccess;
    IODispatchQueue *irqQueue = nullptr;
    kern_return_t queueResult = IODispatchQueue::Create("MacLinuxGPU.Interrupts", 0, 0, &irqQueue);
    if (queueResult != kIOReturnSuccess || !irqQueue)
        return queueResult != kIOReturnSuccess ? queueResult : kIOReturnNoMemory;
    queueResult = driver->SetDispatchQueue("Interrupts", irqQueue);
    irqQueue->release();
    if (queueResult != kIOReturnSuccess) return queueResult;
    if (dext_irq_register(s_token, 0, nullptr, nullptr) != 0)
        return kIOReturnNoResources;
    OSAction *action = nullptr;
    kern_return_t ret = driver->CreateActionInterruptOccurred(sizeof(uint32_t),
                                                               &action);
    if (ret != kIOReturnSuccess || !action)
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    auto *reference = static_cast<uint32_t *>(action->GetReference());
    if (reference) {
        *reference = 0;
        __atomic_store_n(&s_irqDeliver, true, __ATOMIC_RELEASE);
        s_irqReady = dext_irq_bind_action(0, action) == 0;
        if (!s_irqReady) __atomic_store_n(&s_irqDeliver, false, __ATOMIC_RELEASE);
    }
    action->release();
    return s_irqReady ? kIOReturnSuccess : kIOReturnError;
}

kern_return_t
IMPL(MacLinuxGPU, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    if (s_driver || s_dmaQuarantined) {
        MACLINUXGPU_LOG("previous PCI provider still owns shared runtime state");
        return kIOReturnBusy;
    }
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr) {
        MACLINUXGPU_LOG("provider is not an IOPCIDevice");
        return kIOReturnUnsupported;
    }

    // Optional embedded fallback; may be empty and is never assumed complete.
    int fw_result = fw_table_register_embedded();
    if (fw_result != 0) {
        MACLINUXGPU_LOG("firmware registration failed: %d", fw_result);
        return kIOReturnNoMemory;
    }

    IODispatchQueue *bqueue = nullptr;
    kern_return_t qret = IODispatchQueue::Create("MacLinuxGPUBringup", 0, 0, &bqueue);
    if (qret != kIOReturnSuccess || bqueue == nullptr) {
        MACLINUXGPU_LOG("IODispatchQueue::Create failed: %#x", qret);
        return qret != kIOReturnSuccess ? qret : kIOReturnNoMemory;
    }
    s_bringupQueue = bqueue;
    qret = SetDispatchQueue(kIOServiceDefaultQueueName, bqueue);
    if (qret != kIOReturnSuccess) {
        bqueue->release();
        s_bringupQueue = nullptr;
        return qret;
    }

    // Compute sessions become KFD processes when the device supports them;
    // a personality may turn that off ("MacLinuxGPUKFDSessions" = false).
    {
        OSDictionary *properties = nullptr;
        bool kfdSessions = true;
        if (CopyProperties(&properties) == kIOReturnSuccess && properties) {
            if (properties->getObject("MacLinuxGPUKFDSessions") == kOSBooleanFalse)
                kfdSessions = false;
            properties->release();
        }
        dext_compute_set_kfd_policy(kfdSessions);
        MACLINUXGPU_LOG("KFD compute sessions %s", kfdSessions ? "enabled when supported" : "disabled");
    }

    pci->retain();
    s_retainedPCI = pci;
    s_driver = this;
    s_stopping = false;
    dext_compute_set_pci_open(false);

    uint8_t bus = 0, device = 0, function = 0;
    pci->GetBusDeviceFunction(&bus, &device, &function);
    MACLINUXGPU_LOG("matched PCI provider %02x:%02x.%u",
                    (unsigned)bus, (unsigned)device, (unsigned)function);
    for (uint8_t bar = 0; bar < 6; bar++) {
        uint8_t mi = 0; uint64_t sz = 0; uint8_t ty = 0;
        if (pci->GetBARInfo(bar, &mi, &sz, &ty) == kIOReturnSuccess) {
            MACLINUXGPU_LOG("BAR%u memoryIndex=%u size=%llu type=%u",
                            (unsigned)bar, (unsigned)mi,
                            (unsigned long long)sz, (unsigned)ty);
        }
    }

    s_probeAttempted = false;
    s_probeResult = 0;
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_NONE);
    MACLINUXGPU_LOG("driver attached; PCI deferred until a client operation");
    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t
IMPL(MacLinuxGPU, Stop)
{
    if (s_driver != this) return Stop(provider, SUPERDISPATCH);
    if (s_stopping) return kIOReturnSuccess;
    s_stopping = true;
    retain();
    provider->retain();
    s_stopProvider = provider;
    close_session(this);
    // A quarantine that is already provably quiescent must not stall system
    // extension deactivation or upgrade; otherwise FinishSession retries.
    if (s_dmaQuarantined && s_finalCleanup) (void)release_quarantine(this);
    return kIOReturnSuccess;
}

void
MacLinuxGPU::FinishSession()
{
    s_finalCleanup = true;
    MACLINUXGPU_LOG("session close: final cleanup entered (prepared=%d quarantine=%d)",
        s_dmaShutdownPrepared, s_dmaQuarantined);
    // Compute/upstream producers have stopped and IRQ actions are drained.
    // Release final device-managed aliases under the DMA hold before FLR.
    if (s_rtDevice != nullptr && !s_dmaQuarantined) {
        rt_device_free(s_rtDevice);
        s_rtDevice = nullptr;
    }
    if (s_dmaShutdownPrepared && !s_dmaQuarantined) {
        // With every Linux owner gone, the only BAR0 CPU mapping left is the
        // aperture upstream does not unmap after drm_dev_unplug().
        const int aliases = dext_bar0_cpu_release_orphaned();
        if (aliases > 0)
            MACLINUXGPU_LOG("session close: released %d orphaned BAR0 CPU mapping reference(s) after upstream removal", aliases);
        const int isolated = dext_pci_shutdown_reset();
        if (isolated != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_ENDPOINT_ISOLATION, isolated);
            MACLINUXGPU_LOG("session close: endpoint isolation failed (%d); DMA backing retained", isolated);
        }
    }
    dext_compute_set_pci_open(false);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_NONE);
    if (!s_dmaQuarantined) {
        const int released = dext_dma_fini();
        if (released != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_DMA_RETAINED, released);
            MACLINUXGPU_LOG("session close: live DMA backing retained (%d)", released);
        }
    }
    if (s_dmaQuarantined) {
        quarantine_session(this);
        MACLINUXGPU_LOG("session quarantined: retaining clients, provider and runtime owners");
        // Superclass Stop invalidates the provider. Pending Stop requests
        // retain their objects until quiescence can actually be established;
        // a stopping driver releases at once when cached state proves it.
        if (s_stopping) (void)release_quarantine(this);
        return;
    }
    MACLINUXGPU_LOG("session closed after upstream removal, interrupt drain and endpoint isolation");
    complete_session_close(this);
}

void
MacLinuxGPU::FinishStop(IOService *provider)
{
    Stop(provider, SUPERDISPATCH);
    provider->release();
    release();
}

void
IMPL(MacLinuxGPU, InterruptOccurred)
{
    (void)count;
    (void)time;
    if (!action || !__atomic_load_n(&s_irqDeliver, __ATOMIC_ACQUIRE)) return;
    auto *vector = static_cast<uint32_t *>(action->GetReference());
    if (vector != nullptr)
        dext_irq_dispatch((int)*vector);
}

kern_return_t
IMPL(MacLinuxGPU, NewUserClient)
{
    // Observers read cached state only, so they may attach while a session
    // closes or stays quarantined; session clients still may not.
    const bool observer = type == MLG_USER_CLIENT_OBSERVER;
    if (s_stopping || s_driver != this || (s_sessionClosing && !observer))
        return kIOReturnNotAttached;
    if (type != MLG_USER_CLIENT_SESSION && !observer) {
        MACLINUXGPU_LOG("unsupported user-client type %u", (unsigned)type);
        return kIOReturnUnsupported;
    }
    // The codegen'd UserClient class (from MacLinuxGPUUserClient.iig) is
    // OSMetaClass-registered; Create instantiates it (the iig codegen's
    // registry), not `new`.  This is the ONE thing the make build stubs
    // (it returns kIOReturnUnsupported).
    IOService *clientService = nullptr;
    s_creatingObserver = observer;
    kern_return_t ret = Create(this, "MacLinuxGPUUserClientProperties",
                               &clientService);
    s_creatingObserver = false;
    if (ret != kIOReturnSuccess) {
        MACLINUXGPU_LOG("Create UserClient failed: %#x", ret);
        return ret;
    }
    IOUserClient *typed = OSDynamicCast(IOUserClient, clientService);
    if (typed == nullptr) {
        clientService->release();
        MACLINUXGPU_LOG("created service is not an IOUserClient");
        return kIOReturnUnsupported;
    }
    // Start normally recorded the role already; assert it before the client
    // is returned, so no RPC can run with the wrong role.
    auto *created = OSDynamicCast(MacLinuxGPUUserClient, clientService);
    if (created && created->ivars) created->ivars->observer = observer;
    *userClient = typed;
    MACLINUXGPU_LOG("NewUserClient: MacLinuxGPUUserClient created%s",
                    observer ? " (observer)" : "");
    return kIOReturnSuccess;
}

// Driver-lifetime firmware mailbox (see <rt/fw_mailbox.h>).
static IOBufferMemoryDescriptor *s_fwMailbox = nullptr;

static kern_return_t copy_firmware_mailbox(IOMemoryDescriptor **memory)
{
    if (!s_fwMailbox) {
        IOBufferMemoryDescriptor *buffer = nullptr;
        kern_return_t ret = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionInOut, MLG_FW_MAILBOX_TOTAL_SIZE, 16384, &buffer);
        if (ret != kIOReturnSuccess || !buffer)
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        ret = buffer->SetLength(MLG_FW_MAILBOX_TOTAL_SIZE);
        IOAddressSegment range{};
        if (ret == kIOReturnSuccess) ret = buffer->GetAddressRange(&range);
        if (ret != kIOReturnSuccess || !range.address ||
            range.length < MLG_FW_MAILBOX_TOTAL_SIZE ||
            fw_mailbox_attach(reinterpret_cast<void *>(range.address),
                              MLG_FW_MAILBOX_TOTAL_SIZE) != 0) {
            buffer->release();
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        s_fwMailbox = buffer;
        MACLINUXGPU_LOG("firmware mailbox created (%u-byte data window)",
                        (unsigned)MLG_FW_MAILBOX_DATA_SIZE);
    }
    s_fwMailbox->retain();
    *memory = s_fwMailbox;
    return kIOReturnSuccess;
}

void
MacLinuxGPU::free()
{
    // Successful clients and asynchronous cleanup retain this service.
    // Thus no live callback may observe the shared state being released.
    if (s_driver != this) { IOService::free(); return; }
    if (s_bringupQueue != nullptr) {
        s_bringupQueue->release();
        s_bringupQueue = nullptr;
    }
    if (s_retainedPCI != nullptr && !s_dmaQuarantined) {
        s_retainedPCI->release();
        s_retainedPCI = nullptr;
    }
    if (s_fwMailbox != nullptr) {
        // Client mappings keep their own reference to the pages.
        fw_mailbox_detach();
        s_fwMailbox->release();
        s_fwMailbox = nullptr;
    }
    s_driver = nullptr;
    MACLINUXGPU_LOG("driver free: shared bringup storage released");
    IOService::free();
}

// ----------------------------------------------------------------
// MacLinuxGPUUserClient (IOUserClient) — the OSMetaClass method bodies.
//
// The codegen'd OSClassDescription (from MacLinuxGPUUserClient.iig)
// dispatches the UserClient lifecycle + the ExternalMethod to these.
// ----------------------------------------------------------------

kern_return_t
IMPL(MacLinuxGPUUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    MacLinuxGPU *driver = OSDynamicCast(MacLinuxGPU, provider);
    if (driver == nullptr) return kIOReturnUnsupported;
    const bool observer = s_creatingObserver;
    if (s_driver != driver || s_stopping || (s_sessionClosing && !observer))
        return kIOReturnNotAttached;
    IODispatchQueue *ownerQueue = nullptr;
    ret = driver->CopyDispatchQueue(kIOServiceDefaultQueueName, &ownerQueue);
    if (ret != kIOReturnSuccess || ownerQueue == nullptr)
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    // A session client shares the owner's serial queue with every session
    // transition and LSE ioctl. An observer gets its own: SysfsRead and
    // DrmInfo run upstream callbacks that take upstream locks and may sleep
    // on an SMU round trip, so they must neither wait behind an in-flight
    // ioctl nor stall one. Its other selectors hop to the owner's queue.
    IODispatchQueue *clientQueue = ownerQueue;
    if (observer) {
        clientQueue = nullptr;
        ret = IODispatchQueue::Create("MacLinuxGPUObserver", 0, 0, &clientQueue);
        if (ret != kIOReturnSuccess || clientQueue == nullptr) {
            ownerQueue->release();
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
    }
    ret = SetDispatchQueue(kIOServiceDefaultQueueName, clientQueue);
    if (clientQueue != ownerQueue) clientQueue->release();
    if (ret != kIOReturnSuccess || s_nextClientID == UINT64_MAX) {
        ownerQueue->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }
    ivars = IONewZero(MacLinuxGPUUserClient_IVars, 1);
    if (!ivars) { ownerQueue->release(); return kIOReturnNoMemory; }
    ivars->ownerQueue = ownerQueue;
    driver->retain();
    ivars->ownerDriver = driver;
    ivars->clientID = ++s_nextClientID;
    ivars->observer = observer;
    MACLINUXGPU_LOG("UserClient Start (client %llu, type %u)", ivars->clientID,
                    observer ? MLG_USER_CLIENT_OBSERVER : MLG_USER_CLIENT_SESSION);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(MacLinuxGPUUserClient, Stop)
{
    if (!ivars) return Stop(provider, SUPERDISPATCH);
    if (ivars->stopping) return kIOReturnSuccess;
    ivars->stopping = true;
    retain();
    provider->retain();
    ivars->stopProvider = provider;
    if (ivars->observer) {
        // No session membership, handles or mappings: nothing to drain.
        FinishStop(provider);
        return kIOReturnSuccess;
    }
    const bool participant = ivars->sessionGeneration == s_sessionGeneration;
    if (participant && s_participants > 1 && !s_sessionClosing) {
        // IRQ delivery stays active while this client's queues are removed.
        const int released = dext_compute_release_client(ivars->clientID);
        if (released != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_CLIENT_RELEASE, released);
            MACLINUXGPU_LOG("client close: cleanup failed (%d); retaining uncertain shared-session backing", released);
        }
    }
    if (participant) {
        ivars->sessionGeneration = 0;
        if (s_participants) --s_participants;
    }
    if (s_sessionClosing || (participant && (!s_participants || s_dmaQuarantined))) {
        ivars->nextStopping = s_stoppingClients;
        s_stoppingClients = this;
        close_session(ivars->ownerDriver);
    } else {
        FinishStop(provider);
    }
    return kIOReturnSuccess;
}

void
MacLinuxGPUUserClient::FinishStop(IOService *provider)
{
    MacLinuxGPU *driver = ivars->ownerDriver;
    // An observer never holds a lease, and stops on its own queue.
    if (!ivars->observer) s_rawBARLease.release(ivars->clientID);
    if (ivars->ownerQueue) ivars->ownerQueue->release();
    IOSafeDeleteNULL(ivars, MacLinuxGPUUserClient_IVars, 1);
    Stop(provider, SUPERDISPATCH);
    provider->release();
    driver->release();
    release();
}

// AsyncCompletion — IIG dispatch target. The interesting work happens
// inside ExternalMethod's WaitInterrupt case and inside InterruptOccurred;
// this override exists only because IOUserClient demands it.
void
IMPL(MacLinuxGPUUserClient, AsyncCompletion)
{
    (void)action;
    (void)status;
    (void)asyncData;
    (void)asyncDataCount;
}

// The MSI-X interrupt dispatch.  The codegen'd UserClient's
// InterruptOccurred OSAction (the iig codegen's CreateActionInterruptOccurred
// + action->GetReference()) is bound to the dext's IRQ callback (T-irq-dext).
// The make build creates + enables the MSI-X sources but can't bind the
// action (no codegen); this does.
void
IMPL(MacLinuxGPUUserClient, InterruptOccurred)
{
    (void)count;
    (void)time;
    auto *vector = static_cast<uint32_t *>(action->GetReference());
    if (vector != nullptr)
        dext_irq_dispatch((int)*vector);
    // The OSAction is completed implicitly (the codegen completes it on
    // return); no SetResult/Perform (those don't exist on OSAction).
}

// CopyClientMemoryForType — the BAR0..5 memory regions (T-dma-dart-dext).
// type is the BAR index (0..5); the dext returns the IOMemoryDescriptor
// for that BAR (the IOPCIDevice's memory mapping).
kern_return_t
IMPL(MacLinuxGPUUserClient, CopyClientMemoryForType)
{
    if (!ivars || ivars->stopping) return kIOReturnNotAttached;
    if (ivars->observer) return kIOReturnNotPermitted;
    ComputeClientScope clientScope(ivars->clientID);
    if (memory == nullptr || options == nullptr) {
        return kIOReturnBadArgument;
    }
    if (s_retainedPCI == nullptr) {
        return kIOReturnNotReady;
    }
    kern_return_t opened = ensure_open(this);
    if (opened != kIOReturnSuccess) return opened;
    if (type == MLG_FW_MAILBOX_MEMORY_TYPE) {
        *options = 0;
        return copy_firmware_mailbox(memory);
    }
    if (type >= 0x10000) {
        void *cpu = nullptr;
        uint64_t size = 0;
        const int located = dext_compute_bo_memory(type, &cpu, &size);
        IOMemoryDescriptor *descriptor = nullptr;
        if (located == -EAGAIN_L && size) {
            // A KFD process's GTT BO: its TTM pages, at the BO's GPU VA.
            descriptor = copy_bo_ranges_descriptor((uint32_t)type, size);
        } else {
            if (located != 0 || !cpu || !size) return kIOReturnBadArgument;
            descriptor = static_cast<IOMemoryDescriptor *>(dext_dma_copy_descriptor(cpu));
        }
        if (!descriptor) return kIOReturnNotReady;
        *options = 0;
        *memory = descriptor;
        return kIOReturnSuccess;
    }
    if (type > 5) return kIOReturnBadArgument;
    // The reference keeps a raw BAR lease after this RPC returns: writes
    // through the resulting mapping bypass BO ownership and queue checks.
    if (!s_rawBARLease.claim(ivars->clientID,
            ivars->sessionGeneration == s_sessionGeneration, s_participants))
        return kIOReturnBusy;
    uint8_t memoryIndex = 0, barType = 0;
    uint64_t barSize = 0;
    kern_return_t ret = s_retainedPCI->GetBARInfo((uint8_t)type,
                                                   &memoryIndex, &barSize,
                                                   &barType);
    if (ret != kIOReturnSuccess) return ret;
    IOMemoryDescriptor *barMemory = nullptr;
    ret = s_retainedPCI->_CopyDeviceMemoryWithIndex(memoryIndex,
                                                    &barMemory,
                                                    GetProvider());
    if (ret != kIOReturnSuccess || barMemory == nullptr)
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    (void)s_rawBARLease.markMapped(ivars->clientID);
    *options = 0;
    *memory = barMemory;
    return kIOReturnSuccess;
}

// ----------------------------------------------------------------
// The observers' Linux reads, on the observer's own queue. Admission
// guarantees the runtime device and the upstream driver outlive the call.
// ----------------------------------------------------------------
static kern_return_t observer_sysfs_read(IOUserClientMethodArguments *arguments)
{
    const uint64_t *in = arguments->scalarInput;
    uint64_t *out = arguments->scalarOutput;
    const OSData *pathData = arguments->structureInput;
    if (!in || arguments->scalarInputCount != 2 || !out ||
        arguments->scalarOutputCount < MLG_SYSFS_READ_WORDS ||
        arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
        (in[0] != MLG_SYSFS_OP_READ && in[0] != MLG_SYSFS_OP_LIST) ||
        in[1] > (uint64_t)INT64_MAX)
        return kIOReturnBadArgument;
    char path[MLG_SYSFS_PATH_MAX + 1];
    if (!mlg_sysfs_path_copy(path,
                             pathData ? static_cast<const char *>(pathData->getBytesNoCopy()) : nullptr,
                             pathData ? pathData->getLength() : 0, in[0] == MLG_SYSFS_OP_LIST))
        return kIOReturnBadArgument;
    const size_t chunk = arguments->structureOutputMaximumSize < MLG_SYSFS_CHUNK_MAX
        ? (size_t)arguments->structureOutputMaximumSize : MLG_SYSFS_CHUNK_MAX;
    uint8_t buffer[MLG_SYSFS_CHUNK_MAX];
    if (!s_observerReads.enter()) return kIOReturnNotReady;
    size_t length = 0;
    struct kobject *dir = rt_device_kobject(s_rtDevice);
    const long r = in[0] == MLG_SYSFS_OP_READ
        ? linuxu_sysfs_read(dir, path, buffer, chunk, (long long)in[1], &length)
        : linuxu_sysfs_list(dir, path, buffer, chunk, (long long)in[1], &length);
    s_observerReads.leave();
    if (r > 0) {
        arguments->structureOutput = OSData::withBytes(buffer, (size_t)r);
        if (!arguments->structureOutput) return kIOReturnNoMemory;
    }
    out[0] = r < 0 ? (uint64_t)(int64_t)r : 0;
    out[1] = r > 0 ? (uint64_t)r : 0;
    out[2] = length;
    arguments->scalarOutputCount = MLG_SYSFS_READ_WORDS;
    return kIOReturnSuccess;
}

// The render file is opened on first use within a session and closed by
// close_session before the upstream driver is removed.
static struct rt_drm_info *observer_drm(int *error)
{
    struct rt_drm_info *drm = __atomic_load_n(&s_observerDrm, __ATOMIC_ACQUIRE);
    if (drm) return drm;
    *error = rt_drm_info_open(static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)), &drm);
    if (*error) return nullptr;
    struct rt_drm_info *expected = nullptr;
    if (__atomic_compare_exchange_n(&s_observerDrm, &expected, drm, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return drm;
    rt_drm_info_close(drm); // another observer opened it first
    return expected;
}

static kern_return_t observer_drm_info(IOUserClientMethodArguments *arguments)
{
    const uint64_t *in = arguments->scalarInput;
    uint64_t *out = arguments->scalarOutput;
    const OSData *args = arguments->structureInput;
    if (!in || arguments->scalarInputCount != 2 || !out || arguments->scalarOutputCount < 1 ||
        arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
        !in[1] || in[1] > MLG_SYSFS_CHUNK_MAX || arguments->structureOutputMaximumSize < in[1] ||
        (args && args->getLength() > MLG_DRM_INFO_ARGS_MAX) || in[0] > UINT32_MAX)
        return kIOReturnBadArgument;
    if (!rt_drm_info_query_allowed((uint32_t)in[0])) return kIOReturnNotPermitted;
    uint8_t buffer[MLG_SYSFS_CHUNK_MAX];
    if (!s_observerReads.enter()) return kIOReturnNotReady;
    int r = 0;
    struct rt_drm_info *drm = observer_drm(&r);
    if (drm)
        r = rt_drm_info_query(drm, (uint32_t)in[0], args ? args->getBytesNoCopy() : nullptr,
                              args ? args->getLength() : 0, buffer, (uint32_t)in[1]);
    s_observerReads.leave();
    if (!r) {
        arguments->structureOutput = OSData::withBytes(buffer, (size_t)in[1]);
        if (!arguments->structureOutput) return kIOReturnNoMemory;
    }
    out[0] = (uint64_t)(int64_t)r;
    arguments->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

// ----------------------------------------------------------------
// ExternalMethod — the selector-RPC dispatch.
//
// The host app (MacLinuxGPUHostApp) speaks the kMacAMDGPUMethod* numbers
// to the UserClient over the IOUserClient::ExternalMethod RPC.  This
// dispatches each selector to the dext_* C API (the same seam the make
// build's C API uses).  The argument layout follows the IOUserClientMethod
// conventions (the in/out scalar + data buffers).
// ----------------------------------------------------------------
kern_return_t
MacLinuxGPUUserClient::ExternalMethod(uint64_t selector,
                                      IOUserClientMethodArguments *arguments,
                                      const IOUserClientMethodDispatch *dispatch,
                                      OSObject *target,
                                      void *reference)
{
    (void)target;
    (void)reference;
    if (!arguments) return kIOReturnBadArgument;
    if (ivars && ivars->observer && !ivars->onOwnerQueue) {
        // On the observer's own queue. Only the Linux reads run here; the
        // rest reads session state, which lives on the owner's queue.
        if (ivars->stopping) return kIOReturnNotAttached;
        if (!mlg_observer_selector_allowed(selector, arguments->scalarInput,
                                           arguments->scalarInputCount))
            return kIOReturnNotPermitted;
        if (selector == MLG_SELECTOR_SYSFS_READ) return observer_sysfs_read(arguments);
        if (selector == MLG_SELECTOR_DRM_INFO) return observer_drm_info(arguments);
        __block kern_return_t result = kIOReturnNotAttached;
        ivars->onOwnerQueue = true;
        ivars->ownerQueue->DispatchSync(^{
            result = ExternalMethod(selector, arguments, dispatch, target, reference);
        });
        ivars->onOwnerQueue = false;
        return result;
    }
    if (!ivars || ivars->stopping || ivars->ownerDriver != s_driver ||
        (s_stopping && !ivars->observer))
        return kIOReturnNotAttached;
    ComputeClientScope clientScope(ivars->clientID);
    if (!ivars->observer) record_client_identity(this);
    if (ivars->observer) {
        // Cached state and the entitled release only; never PCI or the GPU.
        if (!mlg_observer_selector_allowed(selector, arguments->scalarInput,
                                           arguments->scalarInputCount))
            return kIOReturnNotPermitted;
    } else if (s_sessionClosing && selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodQueryInfo &&
        selector != kMacAMDGPUMethodRuntimeBuild && selector != kMacAMDGPUMethodPing &&
        selector != kMacAMDGPUMethodReleaseQuarantine)
        return kIOReturnBusy;
    if (!ivars->observer && selector >= kMacAMDGPUMethodBOAlloc &&
        selector != kMacAMDGPUMethodQueryInfo &&
        selector != kMacAMDGPUMethodRuntimeBuild &&
        selector != kMacAMDGPUMethodHostWindow &&
        selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodReleaseQuarantine &&
        ivars->sessionGeneration != s_sessionGeneration)
        return kIOReturnNotOpen;

    // Read the in scalars (the IOUserClientMethodArguments layout: scalarInput
    // is the in scalars, scalarOutput is the out scalars).  The mac_amdgpu
    // reference uses arguments->scalarInput / scalarInputCount + scalarOutput /
    // scalarOutputCount + the structureInput/structureOutput data buffers.
    const uint64_t *in  = arguments->scalarInput;
    uint64_t       *out = arguments->scalarOutput;

    // The in/out data buffers (the mac_amdgpu reference uses
    // arguments->structureInput / structureOutput (OSData) + the dispatch
    // check fields).  For the shell, the data buffers carry the struct
    // payloads (the identity / BAR info / DMA pair / firmware blob).
    const OSData *inDataOS  = arguments->structureInput;
    OSData       *outDataOS = arguments->structureOutput;
    const void *inData  = inDataOS ? inDataOS->getBytesNoCopy() : nullptr;
    void       *outData = outDataOS ? const_cast<void *>(outDataOS->getBytesNoCopy()) : nullptr;
    size_t inSize  = inDataOS ? inDataOS->getLength() : 0;
    size_t outSize = outDataOS ? outDataOS->getLength() : 0;

    switch (selector) {

    case kMacAMDGPUMethodPing: {
        if (arguments->scalarInputCount != 0 || out == nullptr ||
            arguments->scalarOutputCount < 1) return kIOReturnBadArgument;
        out[0] = 0xA117AB1Eu;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetIdentity: {
        // out: bus, device, function, vendor, device ID, class code,
        // revision; callers asking for 9 scalars also get the subsystem
        // vendor and subsystem ID. All values are read from config space.
        if (s_retainedPCI == nullptr) return kIOReturnNotReady;
        if (arguments->scalarInputCount != 0 || out == nullptr ||
            arguments->scalarOutputCount < 7) return kIOReturnBadArgument;
        kern_return_t opened = ensure_open(this);
        if (opened != kIOReturnSuccess) return opened;
        uint8_t bus = 0, device = 0, function = 0;
        uint16_t vid = UINT16_MAX, did = UINT16_MAX;
        uint32_t classRev = UINT32_MAX;
        if (s_retainedPCI->GetBusDeviceFunction(&bus, &device, &function) !=
            kIOReturnSuccess) return kIOReturnIOError;
        s_retainedPCI->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vid);
        s_retainedPCI->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &did);
        s_retainedPCI->ConfigurationRead32(kIOPCIConfigurationOffsetRevisionID, &classRev);
        // Report whatever AMD function the personality matched; upstream's
        // PCI ID table decides support when the probe runs.
        if (vid != 0x1002 || classRev == UINT32_MAX)
            return kIOReturnNoDevice;
        out[0] = bus; out[1] = device; out[2] = function;
        out[3] = vid; out[4] = did;
        out[5] = (classRev >> 8) & 0xffffffu;
        out[6] = classRev & 0xffu;
        uint32_t count = 7;
        if (arguments->scalarOutputCount >= 9) {
            uint16_t subVendor = UINT16_MAX, subDevice = UINT16_MAX;
            s_retainedPCI->ConfigurationRead16(kIOPCIConfigurationOffsetSubSystemVendorID, &subVendor);
            s_retainedPCI->ConfigurationRead16(kIOPCIConfigurationOffsetSubSystemID, &subDevice);
            out[7] = subVendor; out[8] = subDevice;
            count = 9;
        }
        arguments->scalarOutputCount = count;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetBARInfo: {
        // Keep the three-scalar ABI used by MacAMDGPUHost.
        if (s_retainedPCI == nullptr) return kIOReturnNotReady;
        if (arguments->scalarInputCount != 1 || in == nullptr || out == nullptr ||
            arguments->scalarOutputCount < 3) return kIOReturnBadArgument;
        uint64_t bar = in[0];
        if (bar > 5) return kIOReturnBadArgument;
        uint8_t mi = 0; uint64_t sz = 0; uint8_t ty = 0;
        if (s_retainedPCI->GetBARInfo((uint8_t)bar, &mi, &sz, &ty) != kIOReturnSuccess) {
            return kIOReturnNotFound;
        }
        out[0] = mi; out[1] = sz; out[2] = ty;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSetupInterrupts: {
        if (arguments->scalarInputCount != 0) return kIOReturnBadArgument;
        kern_return_t ret = ensure_open(this);
        if (ret != kIOReturnSuccess) return ret;
        ret = prepare_interrupts(ivars->ownerDriver);
        if (ret != kIOReturnSuccess) {
            close_session(ivars->ownerDriver);
            return ret;
        }
        if (arguments->scalarOutputCount > 0 && out != nullptr) {
            out[0] = (uint64_t)dext_irq_vector_count();
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodWaitInterrupt: {
        return kIOReturnUnsupported;
    }

    case kMacAMDGPUMethodSetIRQMask: {
        return kIOReturnUnsupported;
    }

    case kMacAMDGPUMethodAllocateDMABuffer: {
        // A dext address cannot be returned as a usable host pointer.
        // The buffer needs a per-client descriptor export first.
        return kIOReturnUnsupported;
    }

    case kMacAMDGPUMethodFreeDMABuffer: {
        return kIOReturnUnsupported;
    }

    case kMacAMDGPUMethodResetDevice: {
        if (arguments->scalarInputCount || arguments->structureInput ||
            arguments->structureInputDescriptor)
            return kIOReturnBadArgument;
        if (s_retainedPCI == nullptr) return kIOReturnNotReady;
        // A raw reset while upstream owns DMA would invalidate live queues.
        if (s_modulesRunning || s_irqReady || s_rawBARLease.hasMappings())
            return kIOReturnBusy;
        kern_return_t ret = ensure_open(this);
        if (ret != kIOReturnSuccess) return ret;
        const int reset = dext_pci_function_reset();
        if (reset == 0) return kIOReturnSuccess;
        if (reset == -16) return kIOReturnBusy;
        if (reset == -95) return kIOReturnUnsupported;
        return kIOReturnIOError;
    }

    case kMacAMDGPUMethodInitDevice: {
        if (arguments->scalarInputCount || arguments->structureInput ||
            arguments->structureInputDescriptor)
            return kIOReturnBadArgument;
        kern_return_t ret = ensure_open(this);
        if (ret != kIOReturnSuccess) return ret;
        if (s_modulesRunning) {
            return dext_compute_start(static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice))) == 0
                ? kIOReturnSuccess : kIOReturnNotReady;
        }
        if (dext_pci_transport_fault() != DEXT_PCI_FAULT_NONE)
            return kIOReturnIOError;

        if (!s_rtDevice) s_rtDevice = rt_device_alloc();
        if (!s_rtDevice) {
            MACLINUXGPU_LOG("initialization: runtime device allocation failed");
            close_session(ivars->ownerDriver);
            return kIOReturnNotReady;
        }
        ret = prepare_interrupts(ivars->ownerDriver);
        if (ret != kIOReturnSuccess) {
            MACLINUXGPU_LOG("initialization: interrupt preparation failed (%#x)", ret);
            close_session(ivars->ownerDriver);
            return ret;
        }

        // Upstream probe can free DMA during its own error unwind, before
        // returning here. Keep that backing pinned until success or FLR.
        const int probeHeld = dext_dma_begin_probe(linuxu_dart_budget());
        if (probeHeld != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_PROBE_HOLD, probeHeld);
            MACLINUXGPU_LOG("initialization: cannot reserve probe DMA backing (%d)", probeHeld);
            close_session(ivars->ownerDriver);
            return kIOReturnNotReady;
        }
        s_probeAttempted = true;
        s_probeResult = linuxu_driver_bootstrap();
        if (s_probeResult == 0) {
            s_modulesRunning = true;
            // PCI driver registration can succeed even when its probe failed.
            s_probeResult = rt_pci_probe_result(rt_device_get_pdev(s_rtDevice));
        }
        if (s_probeResult != 0) {
            MACLINUXGPU_LOG("upstream PCI probe failed: %d", s_probeResult);
            if (rt_pci_probe_cleanup_retained(rt_device_get_pdev(s_rtDevice))) {
                s_dmaQuarantined = true;
                note_quarantine(MLG_QUARANTINE_PROBE_RETAINED, s_probeResult);
                MACLINUXGPU_LOG("upstream failed-probe ownership retained; preserving modules, device and DMA backing");
            }
            close_session(ivars->ownerDriver);
            return kIOReturnError;
        }
        MACLINUXGPU_LOG("upstream AMDGPU PCI probe completed");
        int computeResult = dext_compute_start(
            static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)));
        if (computeResult != 0) {
            MACLINUXGPU_LOG("compute initialization failed: %d", computeResult);
            close_session(ivars->ownerDriver);
            return kIOReturnNotReady;
        }
        const int probeCommitted = dext_dma_commit_probe();
        if (probeCommitted != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_PROBE_COMMIT, probeCommitted);
            MACLINUXGPU_LOG("probe DMA cleanup failed (%d); session quarantined", probeCommitted);
            close_session(ivars->ownerDriver);
            return kIOReturnError;
        }
        s_observerReads.open();
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodLoadFirmware: {
        if (s_modulesRunning) return kIOReturnBusy;
        const auto opened = ensure_open(this);
        if (opened != kIOReturnSuccess) return opened;
        // Preload path: a client reads a file from the installed firmware
        // directory and pushes it before InitDevice.  The in data is
        //   struct { uint32_t name_len; char name[128]; } + blob
        // where name is the exact request_firmware() name, for example
        // "amdgpu/<ip>_<major>_<minor>_<rev>_<suffix>.bin".  Any name is
        // accepted; the on-demand mailbox serves names nobody pushed.
        // Inputs above the inline limit arrive as a memory descriptor.
        IOMemoryMap *inputMap = nullptr;
        const uint8_t *p = static_cast<const uint8_t *>(inData);
        size_t size = inSize;
        if (!p && arguments->structureInputDescriptor) {
            if (arguments->structureInputDescriptor->CreateMapping(
                    kIOMemoryMapReadOnly, 0, 0, 0, 0, &inputMap) != kIOReturnSuccess ||
                !inputMap)
                return kIOReturnNoMemory;
            p = reinterpret_cast<const uint8_t *>(inputMap->GetAddress());
            size = (size_t)inputMap->GetLength();
        }
        kern_return_t status = kIOReturnBadArgument;
        uint32_t name_len = 0;
        if (p && size > 4 + 128) {
            memcpy(&name_len, p, 4);
            if (name_len && name_len <= 128) {
                char name[129];
                memcpy(name, p + 4, name_len);
                name[name_len] = '\0';
                status = fw_table_override(name, p + 4 + 128, size - (4 + 128)) == 0
                    ? kIOReturnSuccess : kIOReturnError;
                if (status == kIOReturnSuccess)
                    MACLINUXGPU_LOG("firmware pushed: %s (%zu bytes)", name,
                                    size - (4 + 128));
            }
        }
        if (inputMap) inputMap->release();
        return status;
    }

    // ---- the COMPUTE selectors (T-dext-userclient-seam).  These route to
    // the dext_compute.* C API (dext_compute.c), whose argument layouts match
    // the mac_amdgpu reference EXACTLY (the ported HSA runtime sends args in
    // the reference's layout).  The real GPU routing is the p3-hw-gate (the
    // dext_compute_dk.mm gpu-op hook); the in-memory state machine is the
    // host-verifiable answer.

    case kMacAMDGPUMethodRuntimeBuild: {
        // out[0]=magic, out[1]=ABI, out[2]=build. No hardware access.
        // Observers and closing sessions get the cached-flag answer, which
        // never reads upstream device state. A fourth scalar, when requested,
        // is the compiled build regardless of readiness.
        if (arguments->scalarInputCount != 0 || arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) return kIOReturnBadArgument;
        uint64_t out[4] = {0, 0, 0, 0};
        if (dext_compute_runtime_build_cached(out) != 0) return kIOReturnError;
        if (!ivars->observer && !s_sessionClosing &&
            dext_compute_runtime_build(out) != 0) return kIOReturnError;
        const uint32_t words = arguments->scalarOutputCount >= 4 ? 4 : 3;
        for (uint32_t i = 0; i < words; ++i) arguments->scalarOutput[i] = out[i];
        arguments->scalarOutputCount = words;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodQueryInfo: {
        // Cached text only: no PCI session claim, MMIO or initialization.
        // in: tag, byte cursor. out: end, next cursor, byte count, packed text.
        if (in && arguments->scalarInputCount == 2 &&
            in[0] == DEXT_COMPUTE_QUERY_KERNEL_LOG) {
            if (!out || arguments->scalarOutputCount < 4)
                return kIOReturnBadArgument;
            uint64_t cursor = in[1], end = 0;
            uint8_t text[104] = {};
            const uint32_t words = arguments->scalarOutputCount < 16
                ? arguments->scalarOutputCount : 16;
            const size_t copied = klog_read(&cursor, (char *)text,
                                             (words - 3) * sizeof(uint64_t), &end);
            memset(out, 0, words * sizeof(uint64_t));
            out[0] = end; out[1] = cursor; out[2] = copied;
            memcpy(out + 3, text, copied);
            arguments->scalarOutputCount = 3 + (uint32_t)((copied + 7) / 8);
            return kIOReturnSuccess;
        }
        // Session lifecycle and quarantine cause: lifecycle variables only.
        if (in && arguments->scalarInputCount == 1 &&
            in[0] == DEXT_COMPUTE_QUERY_SESSION_STATE) {
            if (!out || arguments->scalarOutputCount < MLG_SESSION_STATE_WORDS)
                return kIOReturnBadArgument;
            session_state(out);
            arguments->scalarOutputCount = MLG_SESSION_STATE_WORDS;
            return kIOReturnSuccess;
        }
        // Namespaced diagnostic query; existing MacAMDGPU query tags retain
        // their layouts. This reports probe progress without touching MMIO.
        if (in && arguments->scalarInputCount == 1 &&
            in[0] == DEXT_COMPUTE_QUERY_PROBE_STATUS) {
            if (!out || arguments->scalarOutputCount < 5)
                return kIOReturnBadArgument;
            out[0] = s_probeAttempted;
            out[1] = s_modulesRunning;
            out[2] = (uint64_t)(int64_t)s_probeResult;
            out[3] = (uint64_t)dext_pci_transport_fault();
            out[4] = dext_pci_transport_fault_offset();
            arguments->scalarOutputCount = 5;
            return kIOReturnSuccess;
        }

        // in[0]=info tag; out[N]=type-specific payload (the reference's tags
        // 1/2/3/4/5/6/8).
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        const uint64_t tag = arguments->scalarInput[0];
        uint64_t out[32] = {0};
        int n = dext_compute_query_info(tag, out,
            arguments->scalarOutputCount < 32 ? (int)arguments->scalarOutputCount : 32);
        if (n < 0) {
            if (n == -ENOTREADY_L) return kIOReturnNotReady;
            return kIOReturnBadArgument;
        }
        for (int i = 0; i < n && i < (int)arguments->scalarOutputCount; i++)
            arguments->scalarOutput[i] = out[i];
        arguments->scalarOutputCount = (uint32_t)n;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOAlloc: {
        // Dual ABI (the reference's legacy single-input + v0.1.27 4-input).
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 3)
            return kIOReturnBadArgument;
        const bool legacy = (arguments->scalarInputCount < 4);
        uint64_t size = arguments->scalarInput[0];
        uint32_t domain = legacy ? (uint32_t)DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY
                                 : (uint32_t)arguments->scalarInput[1];
        uint64_t alignment = legacy ? 4096ULL : arguments->scalarInput[2];
        uint64_t flags = legacy ? 0ULL : arguments->scalarInput[3];
        uint64_t h = 0, gpu_va = 0, cpu = 0;
        int r = dext_compute_bo_alloc(size, domain, alignment, flags, &h, &gpu_va, &cpu);
        if (r != 0) {
            if (r == -ENOMEM_L) return kIOReturnNoSpace;
            if (r == -ENOTREADY_L) return kIOReturnNotReady;
            return kIOReturnBadArgument;
        }
        arguments->scalarOutput[0] = h;
        arguments->scalarOutput[1] = gpu_va;
        arguments->scalarOutput[2] = cpu;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOFree: {
        // in[0]=handle
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1)
            return kIOReturnBadArgument;
        int r = dext_compute_bo_free(arguments->scalarInput[0]);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -EAGAIN_L || r == -EBUSY_L) return kIOReturnBusy;
        if (r != 0) return kIOReturnError;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOGetInfo: {
        // in[0]=handle; out[0]=gpu_va, [1]=byte_off, [2]=size, [3]=align, [4]=domain
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 3)
            return kIOReturnBadArgument;
        uint64_t out[5] = {0};
        int r = dext_compute_bo_get_info(arguments->scalarInput[0], out);
        if (r != 0) return kIOReturnBadArgument;
        for (int i = 0; i < 5 && i < (int)arguments->scalarOutputCount; i++)
            arguments->scalarOutput[i] = out[i];
        if (arguments->scalarOutputCount > 5) arguments->scalarOutputCount = 5;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOMap: {
        // in[0]=handle; out[0]=memory_type, [1]=size
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        uint64_t out[2] = {0};
        int r = dext_compute_bo_map(arguments->scalarInput[0], out);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -ENOTREADY_L) return kIOReturnUnsupported;
        if (r != 0) return kIOReturnBadArgument;
        arguments->scalarOutput[0] = out[0];
        if (arguments->scalarOutputCount >= 2) arguments->scalarOutput[1] = out[1];
        if (arguments->scalarOutputCount > 2) arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSubmitIB: {
        // in[0]=cs_handle; out[0]=fence_handle (== cs_handle)
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        uint64_t fence = 0;
        int r = dext_compute_submit_ib(arguments->scalarInput[0], &fence);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r != 0) return kIOReturnError;
        arguments->scalarOutput[0] = fence;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodWaitFence: {
        // in[0]=fence_handle, in[1]=timeout_ns; out[0]=status (0=signaled,1=timeout)
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 2 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        uint64_t wstatus = 99;
        int r = dext_compute_wait_fence(arguments->scalarInput[0],
                                        arguments->scalarInput[1], &wstatus);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r != 0) return kIOReturnNotReady;
        arguments->scalarOutput[0] = wstatus;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodMESAddQueue: {
        // The reference returns kIOReturnUnsupported (persistent user queues
        // require queue removal + retained MQD/wptr BO references that raw CS
        // fencing does not establish).  Match that exactly.
        return kIOReturnUnsupported;
    }

    case kMacAMDGPUMethodCSCreate: {
        // in[0]=ip_type, in[1]=ip_instance; out[0]=handle
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 2 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        uint64_t h = 0;
        int r = dext_compute_cs_create((uint32_t)arguments->scalarInput[0],
                                       (uint32_t)arguments->scalarInput[1], &h);
        if (r == -ENOMEM_L) return kIOReturnNoResources;
        if (r != 0) return kIOReturnBadArgument;
        arguments->scalarOutput[0] = h;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodCSWriteDwords: {
        // in[0]=handle; inData=dwords (4-byte aligned); out[0]=written
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            !arguments->structureInput ||
            arguments->structureInput->getLength() % 4 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 1)
            return kIOReturnBadArgument;
        const uint32_t *dwords = (const uint32_t *)arguments->structureInput->getBytesNoCopy();
        uint32_t count = (uint32_t)(arguments->structureInput->getLength() / 4);
        uint32_t written = 0;
        int r = dext_compute_cs_write_dwords(arguments->scalarInput[0], dwords, count, &written);
        if (r != 0) return kIOReturnBadArgument;
        arguments->scalarOutput[0] = written;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodCSDestroy: {
        // in[0]=handle
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1)
            return kIOReturnBadArgument;
        int r = dext_compute_cs_destroy(arguments->scalarInput[0]);
        if (r != 0) return kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodHostWindow: {
        // in[0]=0 queries; a nonzero value chooses the pre-probe host VA.
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount != 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 3)
            return kIOReturnBadArgument;
        kern_return_t opened = ensure_open(this);
        if (opened != kIOReturnSuccess) return opened;
        uint64_t out[3] = {0};
        int r = dext_compute_host_window(arguments->scalarInput[0], out);
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r != 0) return kIOReturnBadArgument;
        arguments->scalarOutput[0] = out[0];
        arguments->scalarOutput[1] = out[1];
        arguments->scalarOutput[2] = out[2];
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodShutdownGPU: {
        // Preserve the reference's status/phase response. Completion is
        // reported only after asynchronous IRQ drain and upstream removal;
        // callers may retry while status is Busy and phase is 2.
        if (arguments->scalarInputCount != 0 || arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 2) return kIOReturnBadArgument;
        if (s_rawBARLease.hasMappings()) return kIOReturnBusy;
        const bool participant = ivars->sessionGeneration == s_sessionGeneration;
        if (s_participants > (participant ? 1u : 0u)) return kIOReturnBusy;
        if (s_pciOpen && !s_sessionClosing) close_session(ivars->ownerDriver);
        out[0] = s_dmaQuarantined ? kIOReturnError :
                 (s_sessionClosing ? kIOReturnBusy : kIOReturnSuccess);
        out[1] = s_dmaQuarantined ? 5 : (s_sessionClosing ? 2 : 6);
        arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetReBARInfo: {
        // in[0]=bar; out[0..5]=offset/cap/ctl/supported/selected/assigned
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount != 1 ||
            arguments->scalarInput[0] >= 6 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 6)
            return kIOReturnBadArgument;
        uint64_t out[6] = {0};
        int r = dext_compute_get_rebar(arguments->scalarInput[0], out);
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r != 0) return kIOReturnBadArgument;
        for (int i = 0; i < 6; i++) arguments->scalarOutput[i] = out[i];
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodHostMemoryTest: {
        // in[0]=size; out[0..5]=status/stage/mismatches/first/host_gpu/vram_gpu
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr || arguments->scalarOutputCount < 6)
            return kIOReturnBadArgument;
        uint64_t out[6] = {0};
        int r = dext_compute_host_mem_test(arguments->scalarInput[0], out);
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r != 0) return kIOReturnError;
        for (int i = 0; i < 6; i++) arguments->scalarOutput[i] = out[i];
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAQLQueueCreate: {
        // in[0]=ring, in[1]=metadata, in[2]=packets; out[0]=status, [1]=handle
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount != 3 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 2 ||
            arguments->structureInput) return kIOReturnBadArgument;
        uint64_t status = 0, handle = 0;
        int r = dext_compute_aql_queue_create(arguments->scalarInput[0],
                                              arguments->scalarInput[1],
                                              arguments->scalarInput[2], &status, &handle);
        if (r == -EINVAL_L) return kIOReturnBadArgument;
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r == -EAGAIN_L || r == -EBUSY_L) return kIOReturnBusy;
        if (r == -ENOMEM_L) return kIOReturnNoResources;
        if (r != 0) return kIOReturnError;
        arguments->scalarOutput[0] = status;
        arguments->scalarOutput[1] = handle;
        arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAQLQueueKick:
    case kMacAMDGPUMethodAQLQueueDestroy: {
        const bool kick = (selector == kMacAMDGPUMethodAQLQueueKick);
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount != (kick ? 2u : 1u) ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 1 ||
            arguments->structureInput) return kIOReturnBadArgument;
        uint64_t status = 0;
        int r = kick ? dext_compute_aql_queue_kick(arguments->scalarInput[0],
                                                   arguments->scalarInput[1], &status) :
                       dext_compute_aql_queue_destroy(arguments->scalarInput[0], &status);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -EBUSY_L) return kIOReturnBusy;
        if (r != 0) return kIOReturnNotReady;
        arguments->scalarOutput[0] = status;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAQLQueueService: {
        // in[0]=handle; out[0]=status, [1]=inactive
        if (arguments->scalarInput == nullptr || arguments->scalarInputCount != 1 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 2 ||
            arguments->structureInput) return kIOReturnBadArgument;
        uint64_t status = 0, inactive = 0;
        int r = dext_compute_aql_queue_service(arguments->scalarInput[0], &status, &inactive);
        if (r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -EBUSY_L) return kIOReturnBusy;
        if (r != 0) return kIOReturnNotReady;
        arguments->scalarOutput[0] = status;
        arguments->scalarOutput[1] = inactive;
        arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAQLDispatch: {
        // inData=AQLDispatchRequest (208 bytes); out[0..4]=status/completion/stage/inactive/read_index
        if (arguments->scalarInputCount || !arguments->scalarOutput ||
            arguments->scalarOutputCount < 5 || !arguments->structureInput ||
            arguments->structureInput->getLength() != sizeof(amdgpu::AQLDispatchRequest))
            return kIOReturnBadArgument;
        amdgpu::AQLDispatchRequest request{};
        memcpy(&request, arguments->structureInput->getBytesNoCopy(), sizeof(request));
        if (!amdgpu::aql_dispatch_shape(request)) return kIOReturnBadArgument;
        uint64_t out[5] = {0};
        int r = dext_compute_aql_dispatch(&request, sizeof(request), out);
        if (r == -EINVAL_L) return kIOReturnBadArgument;
        if (r == -ENOMEM_L) return kIOReturnNoResources; // every queue slot held
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r == -EBUSY_L) return kIOReturnBusy;
        if (r != 0) return kIOReturnIOError;
        arguments->scalarOutput[0] = out[0];
        arguments->scalarOutput[1] = out[1];
        arguments->scalarOutput[2] = out[2];
        arguments->scalarOutput[3] = out[3];
        arguments->scalarOutput[4] = out[4];
        arguments->scalarOutputCount = 5;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOCopy: {
        if (!in || arguments->scalarInputCount != 5 || !out ||
            arguments->scalarOutputCount < 1 || in[4] == 0 ||
            in[4] > 4 * 1024 * 1024 || arguments->structureInput ||
            arguments->structureInputDescriptor || arguments->structureOutputDescriptor)
            return kIOReturnBadArgument;
        int r = dext_compute_bo_copy(in[0], in[2], in[1], in[3], (uint32_t)in[4]);
        if (r == -EINVAL_L || r == -ENOENT_L) return kIOReturnBadArgument;
        out[0] = r == 0 ? kIOReturnSuccess : kIOReturnIOError;
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOWrite:
    case kMacAMDGPUMethodBORead: {
        const bool write = selector == kMacAMDGPUMethodBOWrite;
        if (!in || arguments->scalarInputCount != 3 ||
            arguments->scalarOutputCount || arguments->structureInputDescriptor ||
            arguments->structureOutputDescriptor || !in[2] || in[2] > 4096 ||
            ((in[1] | in[2]) & 3))
            return kIOReturnBadArgument;
        if (write ? (!inData || inSize != in[2] || arguments->structureOutputMaximumSize)
                  : (inData || arguments->structureOutputMaximumSize < in[2]))
            return kIOReturnBadArgument;
        uint8_t data[4096];
        int r = write ? dext_compute_bo_write(in[0], in[1], inData, (size_t)in[2])
                      : dext_compute_bo_read(in[0], in[1], data, (size_t)in[2]);
        if (r == -EINVAL_L || r == -ENOENT_L) return kIOReturnBadArgument;
        if (r != 0) return kIOReturnIOError;
        if (!write) {
            arguments->structureOutput = OSData::withBytes(data, (size_t)in[2]);
            if (!arguments->structureOutput) return kIOReturnNoMemory;
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodComputeDispatch: {
        if (arguments->scalarInputCount || !out || arguments->scalarOutputCount < 3 ||
            !inData || arguments->structureInputDescriptor ||
            (inSize != amdgpu::kComputeDispatchV1Bytes &&
             inSize != sizeof(amdgpu::ComputeDispatchRequest)))
            return kIOReturnBadArgument;
        amdgpu::ComputeDispatchRequest request{};
        memcpy(&request, inData, inSize);
        if (!amdgpu::compute_dispatch_shape(request, dext_compute_rsrc1_clamp_ieee()) ||
            (request.version == 1 ? inSize != amdgpu::kComputeDispatchV1Bytes
                                  : inSize != sizeof(request)))
            return kIOReturnBadArgument;
        uint64_t result[3]{};
        int r = dext_compute_dispatch(&request, inSize, result);
        if (r == -EINVAL_L || r == -ENOENT_L) return kIOReturnBadArgument;
        if (r == -ENOMEM_L) return kIOReturnNoResources; // every queue slot held
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r != 0) return kIOReturnIOError;
        memcpy(out, result, sizeof(result));
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOExport:
    case kMacAMDGPUMethodBOImport: {
        if (!in || arguments->scalarInputCount != 3 || !out ||
            arguments->scalarOutputCount != 3 || arguments->structureInput ||
            arguments->structureInputDescriptor || arguments->structureOutputDescriptor)
            return kIOReturnBadArgument;
        int r = selector == kMacAMDGPUMethodBOExport ?
            dext_compute_bo_export(in[0], in[1], in[2], out) :
            dext_compute_bo_import(in[0], in[1], in[2], out);
        if (r == -ENOMEM_L) return kIOReturnNoResources;
        if (r == -ENOTREADY_L) return kIOReturnNotReady;
        if (r) return kIOReturnBadArgument;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAtomicRequester: {
        // in[0]=enable; out[0..7]=atomic_requester::Snapshot. No PCI access.
        if (!arguments->scalarInput || arguments->scalarInputCount != 1 ||
            !arguments->scalarOutput ||
            arguments->scalarOutputCount < DEXT_COMPUTE_ATOMIC_REQUESTER_WORDS)
            return kIOReturnBadArgument;
        if (dext_compute_atomic_requester(arguments->scalarInput[0],
                                          arguments->scalarOutput) != 0)
            return kIOReturnBadArgument;
        arguments->scalarOutputCount = DEXT_COMPUTE_ATOMIC_REQUESTER_WORDS;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodReleaseQuarantine: {
        // out[0]=IOReturn status of the release, out[1]=MLG_RELEASE_* blocker.
        if (arguments->scalarInputCount || arguments->structureInput ||
            arguments->structureInputDescriptor || !out ||
            arguments->scalarOutputCount < 2) return kIOReturnBadArgument;
        OSDictionary *entitlements = nullptr;
        bool entitled = false;
        if (CopyClientEntitlements(&entitlements) == kIOReturnSuccess && entitlements) {
            entitled = entitlements->getObject(MLG_SESSION_RELEASE_ENTITLEMENT) == kOSBooleanTrue;
            entitlements->release();
        }
        if (!entitled) return kIOReturnNotPrivileged;
        const uint32_t blocker = release_quarantine(ivars->ownerDriver);
        out[0] = blocker == MLG_RELEASE_READY ? kIOReturnSuccess :
                 (mlg_release_blocker_permanent(blocker) ? kIOReturnError : kIOReturnNotReady);
        out[1] = blocker;
        arguments->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSetIPBase:
    case kMacAMDGPUMethodGetIPBase:
    case kMacAMDGPUMethodLoadDiscoveryBin:
    case kMacAMDGPUMethodSubmitTestPM4:
    case kMacAMDGPUMethodSDMACopyTest:
    case kMacAMDGPUMethodGetDiagnostics:
    case kMacAMDGPUMethodDumpTMR:
    case kMacAMDGPUMethodDumpPSP:
    case kMacAMDGPUMethodDumpCmdBuf:
    case kMacAMDGPUMethodLiveStatus:
    case kMacAMDGPUMethodDisableSmuFeatures:
        return kIOReturnUnsupported;

    default:
        return kIOReturnUnsupported;
    }
}

// ----------------------------------------------------------------
// The OSMetaClass registration.
//
// The iig codegen (the Xcode DriverKit build-phase, run on
// MacLinuxGPU.iig / MacLinuxGPUUserClient.iig) produces:
//   - The derived class headers (MacLinuxGPU.h / MacLinuxGPUUserClient.h)
//     that this file #includes.
//   - The sMetaClass / OSClassDescription method tables (in the
//     codegen'd .iig.cpp) that register the classes with IOKit's class
//     loader, making IOKit instantiate MacLinuxGPU on PCI match + spawn
//     the UserClient per open.
//
// This file provides the method BODIES (the Start/Stop/NewUserClient/
// free/ExternalMethod/... above).  The codegen provides the registration.
// They link together in the Xcode build (the make build does not run the
// codegen; it uses the hand-rolled MacLinuxGPU.mm instead).
// ----------------------------------------------------------------
