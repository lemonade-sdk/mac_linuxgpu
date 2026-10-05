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
#include <time.h>

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
#include <DriverKit/OSArray.h>
#include <DriverKit/OSNumber.h>
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
#include "power_state.h"
#include "raw_bar_lease.h"
#include "session_state.h"
#include "device_properties.h"
#include <rt/bootstrap.h>
#include <rt/cs_selftest.h>
#include <rt/display.h>
#include <rt/surface.h>
#include <rt/drm_info.h>
#include <rt/lx_abi.h>
#include <rt/lx_files.h>
#include <rt/lx_timing.h>
#include <rt/kfd_session.h>
#include <rt/wait_pool.h>
#include <rt/bounded.h>
#include <rt/sysfs.h>
#include <rt/dext_pci.h>
#include <rt/dext_dma.h>
#include <rt/gart.h>
#include <rt/klog.h>
#include <rt/fw_mailbox.h>
#include <rt/power.h>
#include <rt/removal.h>


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
static_assert(MLG_SELECTOR_DISPLAY > MLG_SELECTOR_DRM_SELFTEST &&
              MLG_SELECTOR_DISPLAY < MLG_SELECTOR_LX_FIRST, "display selector");
static_assert(MLG_SELECTOR_EVENT > MLG_SELECTOR_RETIRE &&
              MLG_SELECTOR_EVENT_WAIT < MLG_SELECTOR_LX_FIRST &&
              MLG_EVENT_WAIT_IDS == RT_KFD_WAIT_EVENTS_MAX &&
              MLG_EVENT_WAIT_MAX_MS == RT_KFD_WAIT_MAX_MS &&
              DEXT_COMPUTE_EVENT_CREATE == MLG_EVENT_OP_CREATE &&
              DEXT_COMPUTE_EVENT_DESTROY == MLG_EVENT_OP_DESTROY &&
              DEXT_COMPUTE_EVENT_SET == MLG_EVENT_OP_SET, "event selectors");
static_assert(MLG_SELECTOR_RETIRE > MLG_SELECTOR_DISPLAY &&
              MLG_SELECTOR_RETIRE < MLG_SELECTOR_LX_FIRST, "retire selector");
static_assert(MLG_DISPLAY_PATTERNS == RT_DISPLAY_PATTERNS &&
              MLG_DISPLAY_PATTERN_BARS == RT_DISPLAY_PATTERN_BARS &&
              MLG_DISPLAY_PATTERN_WHITE == RT_DISPLAY_PATTERN_WHITE &&
              MLG_DISPLAY_PATTERN_GRADIENT == RT_DISPLAY_PATTERN_GRADIENT &&
              MLG_DISPLAY_NAME_MAX < RT_DISPLAY_NAME_BYTES &&
              sizeof(struct rt_display_report) <= MLG_DISPLAY_REPORT_MAX &&
              sizeof(struct rt_display_modes) <= MLG_DISPLAY_REPORT_MAX &&
              sizeof(struct rt_surface_verify_result) <= MLG_DISPLAY_REPORT_MAX &&
              sizeof(struct rt_display_present_stats) <= MLG_DISPLAY_REPORT_MAX &&
              sizeof(struct mlg_display_rect) == sizeof(struct rt_surface_rect) &&
              sizeof(struct rt_display_present_stats) == 144 &&
              offsetof(struct mlg_display_present, rect) == 16 &&
              sizeof(struct mlg_display_present) + MLG_DISPLAY_PRESENT_RECTS_MAX *
                  sizeof(struct mlg_display_rect) <= MLG_DISPLAY_PRESENT_BYTES_MAX &&
              sizeof(struct mlg_display_move) == sizeof(struct rt_display_move) &&
              offsetof(struct mlg_display_move, src_y) == offsetof(struct rt_display_move, src_y) &&
              MLG_DISPLAY_PRESENT_MOVES_MAX == RT_DISPLAY_MOVES_MAX, "display ABI");
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
// The unified log takes only events (MACLINUXGPU_EVENT): a dext that logs
// every routine line there gets its logging quarantined by the system for
// high volume, and then the lines that matter after a hang are lost with
// the rest. Routine lines stay in the retained ring (read-driver-log.py).
static void maclinuxgpu_event_sink(const char *text)
{
    os_log(OS_LOG_DEFAULT, "%{public}s", text);
}
#define MACLINUXGPU_LOG(...) maclinuxgpu::RetainedLog(nullptr, __VA_ARGS__)
#define MACLINUXGPU_EVENT(...) \
    maclinuxgpu::RetainedEvent(maclinuxgpu_event_sink, __VA_ARGS__)

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
// Surprise removal: the GPU left the bus (a Thunderbolt unplug).
static bool             s_deviceRemoved = false;
// Retire (session_state.h): an upgrade hands the GPU to the new driver. No
// new session is admitted; with s_retireTerminate this instance asks IOKit
// to terminate it once its session is gone, so its process exits.
static bool             s_retiring = false;
static bool             s_retireTerminate = false;
static bool             s_terminateRequested = false;
static bool             s_creatingObserver = false;
static bool             s_creatingLinuxFile = false;
// DriverKit runs a new user client's Start after NewUserClient returns, not
// inside Create, so the requested role is recorded against the created
// object and claimed by its Start. Both run on the driver's default queue.
static const void      *s_pendingObservers[16];
static const void      *s_pendingLinuxFiles[16];

__attribute__((unused)) static bool pending_role_add(const void **slots, const void *client)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!slots[i]) { slots[i] = client; return true; }
    return false;
}

__attribute__((unused)) static bool pending_role_take(const void **slots, const void *client)
{
    for (unsigned i = 0; i < 16; ++i)
        if (slots[i] == client) { slots[i] = nullptr; return true; }
    return false;
}
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
// Linux-file calls (lx_files) run on their clients' own queues under this
// admission, opened and closed with the observer reads.
static mlg_observer_gate s_lxCalls;
static void observer_display_client_stop(uint64_t clientID);
static uint64_t s_displayOwner; // the client whose OUTPUT is on screen, 0 for none
struct MacLinuxGPUUserClient_IVars {
    MacLinuxGPU *ownerDriver;
    IOService *stopProvider;
    MacLinuxGPUUserClient *nextStopping;
    uint64_t sessionGeneration;
    uint64_t clientID;
    bool stopping;
    bool observer; // read-only: never joins, opens or closes a session
    bool identityRecorded; // pid/name handed to the compute backend
    // Every client's calls arrive on a queue of its own; whatever touches
    // the session runs on the owner's (session) queue, as an async call
    // (owner_call) that never holds the delivery thread.
    IODispatchQueue *ownerQueue;
    // Structure outputs of the client's async session calls, until
    // OWNER_RESULT fetches them (owner_call_store, owner_result).
    IOLock *ownerLock;
    struct OwnerResult *ownerResults;
    uint64_t ownerToken;
    // The memory of its mapped BOs (BOMap), made on the owner's queue, so a
    // mapping (CopyClientMemoryForType, on the delivery thread) only looks
    // one up. Under ownerLock.
    struct ClientMemory *memories;
    bool syncRefusalLogged;
    bool linuxFile; // type 2: a Linux process of its own (lx_files)
    struct rt_lx_client *lx; // created on the first Linux-file call
    MacLinuxGPUUserClient *nextLinuxFile; // s_linuxFiles registry
    // The client's display ops (display_call): the last token issued, and
    // the last op's result for RESULT, in a slot its op's thread shares
    // (the op may finish after the client stopped).
    uint64_t displayToken;
    struct DisplayResultSlot *displayResults;
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
static void client_creator(MacLinuxGPUUserClient *client, int *pidOut, char *name, size_t size)
{
    int pid = 0;
    OSDictionary *properties = nullptr;
    memset(name, 0, size);
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
                while (p[n] && n + 1 < size) { name[n] = p[n]; ++n; }
            } else {
                pid = 0;
            }
        }
        properties->release();
    }
    *pidOut = pid;
}

static void record_client_identity(MacLinuxGPUUserClient *client)
{
    if (!client->ivars || client->ivars->identityRecorded) return;
    client->ivars->identityRecorded = true;
    int pid = 0;
    char name[32] = {};
    client_creator(client, &pid, name, sizeof(name));
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

// ----------------------------------------------------------------
// Linux-file clients (type 2): one Linux process each (rt/lx_files.h),
// created on the client's first Linux-file call. The registry lets a
// session close tear down every process that still exists; a client's
// own Stop takes its process out of it first. Both run upstream file
// release callbacks, so both run while the driver does: a Stop inside
// the call admission, a session close before it removes the driver.
// ----------------------------------------------------------------
static uint32_t s_lxRegistryLock;
static MacLinuxGPUUserClient *s_linuxFiles;

static void lx_registry_acquire()
{
    while (__atomic_exchange_n(&s_lxRegistryLock, 1u, __ATOMIC_ACQUIRE)) {}
}

static void lx_registry_release()
{
    __atomic_store_n(&s_lxRegistryLock, 0u, __ATOMIC_RELEASE);
}

// Unregister @client and hand over its process, if it has one.
static struct rt_lx_client *lx_take(MacLinuxGPUUserClient *client)
{
    struct rt_lx_client *lx;
    lx_registry_acquire();
    lx = client->ivars->lx;
    client->ivars->lx = nullptr;
    for (MacLinuxGPUUserClient **link = &s_linuxFiles; *link; link = &(*link)->ivars->nextLinuxFile) {
        if (*link == client) {
            *link = client->ivars->nextLinuxFile;
            break;
        }
    }
    client->ivars->nextLinuxFile = nullptr;
    lx_registry_release();
    return lx;
}

static void lx_gate_close()
{
    s_lxCalls.close();
    while (!s_lxCalls.drained()) IOSleep(1);
}

// Session close: no Linux-file call may start; every client process exits
// (waits return, files close through upstream postclose); CS self-tests
// whose GPU work completed are torn down. Returns whether GPU work of a
// self-test is still outstanding.
static bool lx_teardown_all()
{
    lx_gate_close();
    // Clients a Stop handed to their own threads (lx_client_stop).
    rt_lx_retire_drain();
    for (;;) {
        struct rt_lx_client *lx = nullptr;
        lx_registry_acquire();
        MacLinuxGPUUserClient *client = s_linuxFiles;
        if (client) {
            s_linuxFiles = client->ivars->nextLinuxFile;
            client->ivars->nextLinuxFile = nullptr;
            lx = client->ivars->lx;
            client->ivars->lx = nullptr;
        }
        lx_registry_release();
        if (!client) break;
        if (lx) rt_lx_client_destroy(lx);
    }
    return rt_cs_selftest_reap() != 0;
}

// Device power (power_state.h); defined after the session paths.
static void power_before_removal();

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
    MACLINUXGPU_EVENT("quarantine cause: step %u code %d (observed by step %u)",
        s_quarantineCause, s_quarantineCode, s_quarantineObserved);
}

static void quarantine_session(MacLinuxGPU *driver)
{
    observer_reads_close();
    lx_gate_close();
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
        MACLINUXGPU_EVENT("quarantine: PCI isolation unconfirmed (%d); retaining all owners", isolated);
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
    if (!dext_compute_quiescent() || rt_cs_selftest_parked()) return MLG_RELEASE_COMPUTE_RETAINED;
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
    if (s_deviceRemoved) flags |= MLG_SESSION_FLAG_DEVICE_REMOVED;
    if (s_retiring) flags |= MLG_SESSION_FLAG_RETIRING;
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

// Nothing of a session exists: no PCI claim, interrupt source, DMA hold,
// upstream driver, runtime device, participant or pending client Stop.
static bool session_idle()
{
    return !s_pciOpen && !s_sessionClosing && !s_dmaQuarantined && !s_dmaShutdownPrepared &&
           !s_modulesRunning && !s_rtDevice && !s_irqReady && !s_participants &&
           !s_stoppingClients && !s_rawBARLease.hasMappings();
}

// Retire TERMINATE: with the session gone, ask IOKit to terminate this
// instance. IOKit stops its clients, then the driver (MacLinuxGPU::Stop
// finds nothing left and finishes at once), and the process exits, which
// lets macOS attach the replacement driver to the GPU.
static void request_termination(MacLinuxGPU *driver)
{
    if (s_terminateRequested || s_stopping || !session_idle()) return;
    s_terminateRequested = true;
    MACLINUXGPU_LOG("retire: no session left; asking IOKit to terminate this driver instance "
                    "so its replacement can attach");
    const kern_return_t ret = driver->Terminate(0);
    if (ret != kIOReturnSuccess) {
        s_terminateRequested = false;
        MACLINUXGPU_EVENT("retire: Terminate failed (%#x); the idle instance stays until its "
                        "device leaves or the Mac restarts", ret);
    }
}

// Shared tail once the endpoint was reset with bus mastering off and every
// DMA descriptor released: close the provider normally and finish Stops.
static void complete_session_close(MacLinuxGPU *driver)
{
    if (s_deviceRemoved) {
        const int closed = dext_pci_close_removed();
        MACLINUXGPU_EVENT("removal: provider %s (%d); the next device probes afresh",
                        closed ? "close deferred" : "closed", closed);
        s_deviceRemoved = false;
    } else {
        dext_close();
    }
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
        return;
    }
    if (s_retireTerminate) request_termination(driver);
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
        MACLINUXGPU_EVENT("quarantine release refused: blocker %u%s", blocker,
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
        MACLINUXGPU_EVENT("quarantine release failed (%d); restart required, do not kill the driver", result);
        return MLG_RELEASE_RESET_FAILED;
    }
    s_dmaQuarantined = false;
    MACLINUXGPU_EVENT("quarantine released after endpoint reset; provider closed");
    complete_session_close(driver);
    return MLG_RELEASE_READY;
}

// Device power (power_state.h): the session is lost with the device.
static void power_device_removed();

// Surprise removal, as Linux handles it (pci_dev_set_disconnected, then
// amdgpu_pci_remove with the device gone): from the moment the device stops
// answering, nothing touches it again (no MMIO, configuration access, reset
// or isolation), GPU work completes with -ECANCELED (rt/removal.h), and the
// session closes all the way: a device that is gone can reach no memory, so
// nothing needs keeping and the provider is closed instead of held.
static void note_device_removed(const char *where)
{
    if (s_deviceRemoved) return;
    s_deviceRemoved = true;
    MACLINUXGPU_EVENT("device removed from the bus (%s): no further hardware access; "
                    "the session closes without waiting for the GPU", where);
    dext_pci_mark_removed();
    if (s_rtDevice != nullptr) {
        const int begun = rt_removal_begin(static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)));
        if (begun) MACLINUXGPU_EVENT("removal: GPU work completion thread not started (%d)", begun);
    }
    dext_compute_device_removed();
    const int kept = dext_dma_device_removed();
    if (kept) MACLINUXGPU_EVENT("removal: %d DMA descriptor(s) could not be completed; backing retained", kept);
    power_device_removed();
}

// Whether the device is still on the bus; once it is not, removal begins.
// Asked when a client or the driver stops: an unplug terminates the PCI
// provider and its clients.
static bool device_removed(const char *where)
{
    if (!s_deviceRemoved && s_pciOpen && !dext_pci_device_present())
        note_device_removed(where);
    return s_deviceRemoved;
}

// What the display ops hold, released while the upstream driver runs: a
// showing pattern or output (its configuration committed again, or, for a
// removed device, nothing committed: rt/display.h), then the imported
// surfaces, whose client mappings follow once the GPU can no longer reach
// them (retired by the DMA hold until the endpoint reset when that is
// later, released at once for a removed device).
static void release_display_owners(const char *why)
{
    if (rt_display_showing()) {
        MACLINUXGPU_LOG("%s: turning the display pattern or output off%s", why,
                        s_deviceRemoved ? " (device removed: nothing is committed)" : "");
        rt_display_stop();
    }
    __atomic_store_n(&s_displayOwner, 0, __ATOMIC_RELEASE);
    if (rt_surface_count()) {
        const unsigned released = rt_surface_remove_all();
        MACLINUXGPU_LOG("%s: released %u imported surface(s)", why, released);
    }
}

// The session's connectors leave this service's properties (displays_publish).
static void displays_unpublish(MacLinuxGPU *driver);
static void close_session(MacLinuxGPU *driver)
{
    if (s_sessionClosing) return;
    s_sessionClosing = true;
    s_finalCleanup = false;
    // No observer read may run an upstream callback past this point.
    observer_reads_close();
    displays_unpublish(driver);
    // The display goes first. A quarantined session keeps every upstream
    // owner, the display's included, unless its device is gone.
    if (s_modulesRunning && (!s_dmaQuarantined || s_deviceRemoved))
        release_display_owners("session close");
    // Linux-file processes exit while the driver runs; a self-test whose
    // GPU work never completed leaves that work uncertain.
    if (lx_teardown_all() && !s_deviceRemoved) {
        s_dmaQuarantined = true;
        note_quarantine(MLG_QUARANTINE_COMPUTE_UNCERTAIN, -16);
    }
    MACLINUXGPU_EVENT("session close begin: probe=%d result=%d modules=%d pci=%d quarantine=%d participants=%u",
        s_probeAttempted, s_probeResult, s_modulesRunning, s_pciOpen,
        s_dmaQuarantined, s_participants);
    // Stop is also reached through forced service termination. It provides
    // no proof that a client's raw BAR mappings have been revoked yet.
    if (s_rawBARLease.hasMappings() && s_deviceRemoved) {
        // The mapped BAR belongs to a device that is gone; it carries no DMA.
        MACLINUXGPU_EVENT("session close: a client still maps a BAR of the removed device");
    } else if (s_rawBARLease.hasMappings()) {
        s_dmaQuarantined = true;
        note_quarantine(MLG_QUARANTINE_RAW_BAR_MAPPING, 0);
        MACLINUXGPU_LOG("session close: raw BAR mapping lifetime uncertain; retaining backing");
    }
    if (s_pciOpen && !s_dmaQuarantined && !s_deviceRemoved) {
        const int held = dext_dma_begin_shutdown();
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
        if (stopped != 0 && s_deviceRemoved) {
            MACLINUXGPU_EVENT("session close: compute stop after removal returned %d; nothing can run", stopped);
        } else if (stopped != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_COMPUTE_UNCERTAIN, stopped);
            MACLINUXGPU_LOG("session close: GPU completion uncertain (%d); retaining runtime", stopped);
        }
    }
    if (s_modulesRunning && !s_dmaQuarantined) {
        // A KFD suspend this driver holds goes back before upstream removal
        // takes its own (power_before_removal).
        power_before_removal();
        // The observers' render file closes like any client's, first.
        if (auto *drm = __atomic_exchange_n(&s_observerDrm, nullptr, __ATOMIC_ACQ_REL))
            rt_drm_info_close(drm);
        // A removed device's GPU work stops being completed here; upstream
        // removal finishes the rest itself (amdgpu_fence_driver_hw_fini).
        if (s_deviceRemoved) rt_removal_end();
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
        MACLINUXGPU_EVENT("session close blocked: interrupt cancellation failed (%d)", drained);
    }
}

// A session already closing or quarantined when the removal was seen: the
// device is gone, so what the quarantine kept for it is free. Finish what
// the close left (compute, upstream removal, runtime device, DMA) and close
// the provider. Waits for an interrupt drain still pending (FinishSession
// calls this then); keeps the quarantine only when interrupt sources could
// not be cancelled, as the provider cannot close under live callbacks.
static void release_removed(MacLinuxGPU *driver)
{
    if (!s_deviceRemoved) return;
    if (!s_finalCleanup) {
        MACLINUXGPU_LOG("removal: waiting for the interrupt drain before releasing the session");
        return;
    }
    if (s_irqDrainFailed) {
        MACLINUXGPU_EVENT("removal: interrupt sources still own callbacks; the session stays quarantined");
        return;
    }
    s_dmaQuarantined = false;
    if (s_modulesRunning) {
        release_display_owners("removal");
        const int stopped = dext_compute_stop();
        if (stopped) MACLINUXGPU_LOG("removal: compute stop returned %d; nothing can run", stopped);
        rt_removal_end();
        if (auto *drm = __atomic_exchange_n(&s_observerDrm, nullptr, __ATOMIC_ACQ_REL))
            rt_drm_info_close(drm);
        MACLINUXGPU_LOG("removal: removing upstream driver");
        linuxu_driver_shutdown();
        s_modulesRunning = false;
    }
    if (s_rtDevice != nullptr) {
        rt_device_free(s_rtDevice);
        s_rtDevice = nullptr;
    }
    (void)dext_bar0_cpu_release_orphaned();
    (void)dext_dma_device_removed();
    const int released = dext_dma_fini();
    if (released)
        MACLINUXGPU_EVENT("removal: DMA backing still owned (%d) is kept; the provider closes", released);
    dext_compute_set_pci_open(false);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_NONE);
    MACLINUXGPU_EVENT("removal: session released after the device left the bus");
    complete_session_close(driver);
}

// Retire (session_state.h): what an upgrade asks of the running driver. On
// the owner's queue. @others counts session clients besides the caller.
// Only the normal close runs; a step whose outcome is uncertain quarantines
// as it would for any close, and nothing here adds a reason to.
static void retire_driver(MacLinuxGPU *driver, uint64_t op, bool force, uint32_t others,
                          uint64_t out[MLG_RETIRE_WORDS])
{
    out[0] = kIOReturnSuccess;
    out[2] = 0;
    if (op == MLG_RETIRE_OP_RESUME) {
        if (s_stopping || s_terminateRequested) {
            out[0] = kIOReturnNotPermitted;
            out[1] = s_stopping ? MLG_RETIRE_STOPPING : MLG_RETIRE_TERMINATING;
            return;
        }
        if (s_retiring) MACLINUXGPU_LOG("retire: cancelled; new sessions are admitted again");
        s_retiring = s_retireTerminate = false;
        out[1] = MLG_RETIRE_RESUMED;
        return;
    }
    if (s_stopping) { out[1] = MLG_RETIRE_STOPPING; return; }
    if (s_terminateRequested) { out[1] = MLG_RETIRE_TERMINATING; return; }
    const bool terminate = op == MLG_RETIRE_OP_TERMINATE;
    if (s_dmaQuarantined) {
        // Only a provably quiescent quarantine is released, and only by the
        // normal release; anything else needs a restart, never a kill.
        const bool wasRetiring = s_retiring, wasTerminate = s_retireTerminate;
        s_retiring = true;
        s_retireTerminate = wasTerminate || terminate;
        const uint32_t blocker = release_quarantine(driver);
        if (blocker != MLG_RELEASE_READY) {
            s_retiring = wasRetiring;
            s_retireTerminate = wasTerminate;
            out[0] = mlg_release_blocker_permanent(blocker) ? kIOReturnError : kIOReturnNotReady;
            out[1] = MLG_RETIRE_QUARANTINED;
            out[2] = blocker;
            return;
        }
    } else if (s_sessionClosing) {
        // A close already runs (a client exit, a sleep): follow it.
        s_retiring = true;
        s_retireTerminate = s_retireTerminate || terminate;
        out[0] = kIOReturnNotReady;
        out[1] = MLG_RETIRE_CLOSING;
        return;
    } else if (s_pciOpen) {
        if (s_rawBARLease.hasMappings()) {
            out[0] = kIOReturnBusy;
            out[1] = MLG_RETIRE_RAW_BAR;
            return;
        }
        if (others && !force) {
            out[0] = kIOReturnBusy;
            out[1] = MLG_RETIRE_CLIENTS;
            out[2] = others;
            return;
        }
        s_retiring = true;
        s_retireTerminate = s_retireTerminate || terminate;
        MACLINUXGPU_LOG("retire: closing the session for a driver upgrade (%u other client(s))", others);
        close_session(driver);
        if (s_dmaQuarantined) {
            const uint32_t blocker = release_blocker();
            out[0] = mlg_release_blocker_permanent(blocker) ? kIOReturnError : kIOReturnNotReady;
            out[1] = MLG_RETIRE_QUARANTINED;
            out[2] = blocker;
            return;
        }
        out[0] = kIOReturnNotReady;
        out[1] = MLG_RETIRE_CLOSING;
        return;
    }
    // No session (left): idle, and terminated when asked.
    if (!s_retiring) MACLINUXGPU_LOG("retire: no session; new sessions are refused");
    s_retiring = true;
    s_retireTerminate = s_retireTerminate || terminate;
    if (s_retireTerminate) {
        request_termination(driver);
        if (!s_terminateRequested && !s_stopping) {
            out[0] = kIOReturnError;
            out[1] = MLG_RETIRE_IDLE;
            return;
        }
        out[1] = s_stopping ? MLG_RETIRE_STOPPING : MLG_RETIRE_TERMINATING;
        return;
    }
    out[1] = MLG_RETIRE_IDLE;
}

static kern_return_t ensure_open(MacLinuxGPUUserClient *client)
{
    if (!client->ivars || !client->ivars->ownerDriver || s_stopping ||
        client->ivars->stopping || !s_retainedPCI)
        return kIOReturnNotAttached;
    if (client->ivars->observer) return kIOReturnNotPermitted;
    // Retiring for an upgrade: the replacement driver takes the next session.
    if (s_deviceRemoved || s_retiring) return kIOReturnNotAttached;
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

// ----------------------------------------------------------------
// The GPU's identity and its monitors as properties of this service
// (device_identity.h): System Information reads model, VRAM,totalMB,
// ATY,EFIVersionB and rom-revision from here; our tools read
// MacLinuxGPUDevice and MacLinuxGPUDisplays. Identity is set at Start from
// the provider's PCI registers and again after the upstream probe;
// displays follow the display operations and are removed with the session.
// ----------------------------------------------------------------
static maclinuxgpu::DeviceIdentity s_identity;
static maclinuxgpu::DisplayState s_displayReport;    // last report published (monitors not read)
static maclinuxgpu::DisplayState s_displayPublished; // what MacLinuxGPUDisplays holds
static bool s_displaysPublished = false;

static bool provider_register(OSDictionary *properties, const char *key, uint32_t &value)
{
    OSData *data = OSDynamicCast(OSData, properties->getObject(key));
    return data && maclinuxgpu::pci_register_value(data->getBytesNoCopy(), data->getLength(), value);
}

// The provider's configuration registers as IOPCIFamily published them,
// and its current link (IOPCIExpressLinkStatus). No configuration access.
static void identity_read_provider(IOService *provider)
{
    OSDictionary *properties = nullptr;
    if (!provider || provider->CopyProperties(&properties) != kIOReturnSuccess || !properties) return;
    uint32_t vendor = 0, device = 0, revision = 0, subsystemVendor = 0, subsystem = 0;
    if (provider_register(properties, "vendor-id", vendor) &&
        provider_register(properties, "device-id", device) &&
        provider_register(properties, "revision-id", revision)) {
        s_identity.pci = true;
        s_identity.vendor = (uint16_t)vendor;
        s_identity.device = (uint16_t)device;
        s_identity.revision = (uint8_t)revision;
        if (provider_register(properties, "subsystem-vendor-id", subsystemVendor) &&
            provider_register(properties, "subsystem-id", subsystem)) {
            s_identity.subsystemVendor = (uint16_t)subsystemVendor;
            s_identity.subsystem = (uint16_t)subsystem;
        }
    }
    if (OSNumber *link = OSDynamicCast(OSNumber, properties->getObject("IOPCIExpressLinkStatus"))) {
        s_identity.link = true;
        s_identity.linkStatus = link->unsigned64BitValue();
    }
    properties->release();
}

static void identity_publish(MacLinuxGPU *driver, const char *when)
{
    if (!driver) return;
    OSDictionary *properties = maclinuxgpu::identity_properties(s_identity);
    if (!properties) {
        MACLINUXGPU_LOG("identity (%s): no memory for the properties", when);
        return;
    }
    const kern_return_t ret = driver->SetProperties(properties);
    properties->release();
    const char *name = maclinuxgpu::product_name(s_identity, nullptr);
    if (s_identity.probed) {
        MACLINUXGPU_LOG("identity (%s): %s, %u MB %s, VBIOS %s %s, %s -> %#x", when,
                        name ? name : "no product name", maclinuxgpu::vram_total_mb(s_identity),
                        s_identity.driver.vram_type_name, s_identity.driver.vbios_pn,
                        s_identity.driver.vbios_version,
                        s_identity.driver.gfx_target[0] ? s_identity.driver.gfx_target : "no KFD target",
                        ret);
    } else {
        MACLINUXGPU_LOG("identity (%s): %04x:%04x rev %02x, %s -> %#x", when,
                        (unsigned)s_identity.vendor, (unsigned)s_identity.device,
                        (unsigned)s_identity.revision, name ? name : "no product name", ret);
    }
}

// After a successful upstream probe: what the amdgpu device knows.
static void identity_after_probe(MacLinuxGPU *driver, struct pci_dev *pdev)
{
    identity_read_provider(s_retainedPCI);
    struct rt_device_identity driverIdentity;
    const int r = rt_device_identity(pdev, &driverIdentity);
    if (r != 0) {
        MACLINUXGPU_LOG("identity: upstream device not readable (%d)", r);
        return;
    }
    s_identity.driver = driverIdentity;
    s_identity.probed = true;
    identity_publish(driver, "probe");
}

// The connectors of @report as MacLinuxGPUDisplays, when they changed. Runs
// inside the observer admission (the upstream driver is alive) and under
// s_displayRunning (one display operation at a time).
static void displays_publish(struct pci_dev *pdev, const struct rt_display_report &report)
{
    MacLinuxGPU *driver = s_driver;
    if (!driver) return;
    maclinuxgpu::DisplayState seen;
    maclinuxgpu::display_state(report, [](const char *, struct rt_display_monitor &) { return -1; },
                               seen);
    if (s_displaysPublished && maclinuxgpu::display_state_equal(seen, s_displayReport)) return;
    maclinuxgpu::DisplayState state;
    maclinuxgpu::display_state(report, [pdev](const char *name, struct rt_display_monitor &monitor) {
        return rt_display_monitor(pdev, name, &monitor);
    }, state);
    OSDictionary *properties = maclinuxgpu::display_properties(state);
    if (!properties) return;
    const kern_return_t ret = driver->SetProperties(properties);
    properties->release();
    if (ret != kIOReturnSuccess) {
        MACLINUXGPU_LOG("displays: publishing the connectors failed (%#x)", ret);
        return;
    }
    s_displayReport = seen;
    if (!s_displaysPublished || !maclinuxgpu::display_state_equal(state, s_displayPublished)) {
        for (uint32_t i = 0; i < state.count; ++i) {
            const maclinuxgpu::ConnectorState &c = state.connector[i];
            if (c.status == 1)
                MACLINUXGPU_LOG("displays: %s connected%s%s%s", c.name, c.monitor[0] ? " (" : "",
                                c.monitor, c.monitor[0] ? ")" : "");
        }
    }
    s_displayPublished = state;
    s_displaysPublished = true;
}

// The session closes: its connectors are no longer known.
static void displays_unpublish(MacLinuxGPU *driver)
{
    if (!s_displaysPublished || !driver) return;
    s_displaysPublished = false;
    OSString *key = OSString::withCString(maclinuxgpu::kMLGDisplays);
    if (!key) return;
    (void)driver->RemoveProperty(key);
    key->release();
}

// ----------------------------------------------------------------
// Device power (power_state.h has the transitions, the upstream paths and
// the client protocol). Every transition runs on the shared default queue,
// serialized with the session transitions and session-client selectors;
// only the acknowledgement deadline's watcher and a client's waits touch
// state from elsewhere, each under its own atomic or lock.
// ----------------------------------------------------------------
static struct mlg_power s_power;
static uint64_t s_powerStartNs;
static uint64_t s_powerHolders[16];          // clients holding a PREPARE
static IODispatchQueue *s_powerQueue;        // the acknowledgement deadline's watcher
static uint64_t s_powerAckPending;           // serial of the change awaiting its ack, 0 if none
static uint64_t s_powerAckSerial;
static uint32_t s_powerAckFlags;
static bool s_powerAckOnClose;               // the pending ack waits for a session close
static uint32_t s_powerCheckQueued;
struct PowerWaiter {
    MacLinuxGPUUserClient *client;
    OSAction *action;
    uint64_t generation;
};
static PowerWaiter s_powerWaiters[16];
static uint32_t s_powerWaitLock;

static const char *power_state_name(uint32_t state)
{
    static const char *const names[] = {"active", "suspending", "suspended", "resuming", "lost"};
    return state < 5 ? names[state] : "unknown";
}

static uint64_t power_now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// A compute session with the upstream driver running, that a transition
// may act on.
static bool power_session_open()
{
    return s_modulesRunning && s_pciOpen && s_rtDevice && !s_sessionClosing && !s_dmaQuarantined;
}

static struct pci_dev *power_pdev()
{
    return s_rtDevice ? static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)) : nullptr;
}

static void power_wait_acquire()
{
    while (__atomic_exchange_n(&s_powerWaitLock, 1u, __ATOMIC_ACQUIRE)) {}
}

static void power_wait_release()
{
    __atomic_store_n(&s_powerWaitLock, 0u, __ATOMIC_RELEASE);
}

static void power_snapshot(uint64_t out[MLG_POWER_STATE_WORDS])
{
    mlg_power_snapshot(&s_power, out);
    if (__atomic_load_n(&s_powerAckPending, __ATOMIC_ACQUIRE)) out[3] |= MLG_POWER_FLAG_ACK_PENDING;
}

static void power_complete_wait(const PowerWaiter &waiter, kern_return_t status)
{
    IOUserClientAsyncArgumentsArray data = {};
    data[0] = s_power.state;
    data[1] = s_power.generation;
    data[2] = s_power.flags;
    waiter.client->AsyncCompletion(waiter.action, status, data, 3);
    waiter.action->release();
    waiter.client->release();
}

// Complete every wait whose generation is no longer current.
static void power_notify()
{
    PowerWaiter ready[16];
    unsigned count = 0;
    power_wait_acquire();
    for (auto &waiter : s_powerWaiters) {
        if (!waiter.client || waiter.generation == s_power.generation) continue;
        ready[count++] = waiter;
        waiter = {};
    }
    power_wait_release();
    for (unsigned i = 0; i < count; ++i) power_complete_wait(ready[i], kIOReturnSuccess);
}

static kern_return_t power_wait(MacLinuxGPUUserClient *client, OSAction *action, uint64_t known)
{
    PowerWaiter waiter = {client, action, known};
    client->retain();
    action->retain();
    if (known != s_power.generation) {
        power_complete_wait(waiter, kIOReturnSuccess);
        return kIOReturnSuccess;
    }
    power_wait_acquire();
    for (auto &slot : s_powerWaiters) {
        if (slot.client) continue;
        slot = waiter;
        power_wait_release();
        return kIOReturnSuccess;
    }
    power_wait_release();
    action->release();
    client->release();
    return kIOReturnNoResources;
}

// A closing client's waits end now (any queue).
static void power_cancel_waits(MacLinuxGPUUserClient *client)
{
    PowerWaiter cancelled[16];
    unsigned count = 0;
    power_wait_acquire();
    for (auto &waiter : s_powerWaiters) {
        if (waiter.client != client) continue;
        cancelled[count++] = waiter;
        waiter = {};
    }
    power_wait_release();
    for (unsigned i = 0; i < count; ++i) power_complete_wait(cancelled[i], kIOReturnAborted);
}

static void power_set(uint32_t state, uint32_t cause, int error)
{
    const uint32_t from = s_power.state;
    if (state == MLG_POWER_SUSPENDING || state == MLG_POWER_RESUMING) {
        if (!s_powerStartNs) s_powerStartNs = power_now_ns();
    } else if (s_powerStartNs) {
        s_power.last_us = (power_now_ns() - s_powerStartNs) / 1000;
        s_powerStartNs = 0;
    }
    s_power.session = s_sessionGeneration;
    if (!mlg_power_set(&s_power, state, cause, error)) {
        MACLINUXGPU_LOG("power: no transition %s -> %s (cause %u)", power_state_name(from),
                        power_state_name(state), cause);
        return;
    }
    MACLINUXGPU_LOG("power: %s -> %s (cause %u, error %d, flags %#x, holds %u, generation %llu%s)",
                    power_state_name(from), power_state_name(state), cause, error, s_power.flags,
                    s_power.holds, s_power.generation,
                    (s_power.flags & MLG_POWER_FLAG_VRAM_PRESERVED) ? ", VRAM preserved" : ", VRAM lost");
    power_notify();
}

static bool power_device_present()
{
    uint16_t vendor = UINT16_MAX;
    if (!s_retainedPCI) return false;
    s_retainedPCI->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendor);
    return vendor == 0x1002;
}

// A transition that failed: the session goes, so the next client starts a
// new one. A failed quiesce first gives its suspend back to upstream, which
// maps the queues again, so the close removes them through MES rather than
// freeing memory MES may still be using; if the GPU cannot prove that, the
// close keeps the session quarantined as it would for any client.
static void power_fail(MacLinuxGPU *driver, uint32_t cause, int error)
{
    if (s_power.flags & MLG_POWER_FLAG_KFD_QUIESCED) {
        s_power.flags &= ~MLG_POWER_FLAG_KFD_QUIESCED;
        const int resumed = rt_power_resume(power_pdev(), nullptr);
        MACLINUXGPU_LOG("power: KFD suspend handed back before the close (%d)", resumed);
    }
    power_set(MLG_POWER_LOST, cause, error);
    MACLINUXGPU_EVENT("power: closing the compute session (cause %u, error %d); the next client re-probes",
                    cause, error);
    close_session(driver);
}

// No session to act on: stop admitting GPU work, or admit it again.
static void power_idle(uint32_t cause)
{
    power_set(MLG_POWER_SUSPENDING, cause, 0);
    power_set(MLG_POWER_SUSPENDED, cause, 0);
}

static void power_wake_idle(uint32_t cause)
{
    power_set(MLG_POWER_RESUMING, cause, 0);
    power_set(MLG_POWER_ACTIVE, cause, 0);
}

// Upstream KFD suspend (rt_power_quiesce -> kgd2kfd_suspend): VRAM kept.
static void power_quiesce(MacLinuxGPU *driver, uint32_t cause)
{
    power_set(MLG_POWER_SUSPENDING, cause, 0);
    struct rt_power_report report = {};
    const int r = rt_power_quiesce(power_pdev(), &report);
    if (r == -19 /* ENODEV: no KFD bound, legacy queues only */) {
        MACLINUXGPU_LOG("power: no KFD device to quiesce; admission closed only");
        power_set(MLG_POWER_SUSPENDED, cause, 0);
        return;
    }
    // From here this driver holds upstream's suspend (it counts suspends),
    // whether or not every queue came off MES.
    s_power.flags |= MLG_POWER_FLAG_KFD_QUIESCED;
    if (r && r != -37 /* EALREADY: already held */) {
        MACLINUXGPU_EVENT("power: upstream KFD suspend failed (%d): %u of %u queues mapped, "
                        "%u processes marked for reset", r, report.active, report.queues,
                        report.reset_marked);
        power_fail(driver, MLG_POWER_CAUSE_QUIESCE_FAILED, r);
        return;
    }
    ++s_power.quiesces;
    MACLINUXGPU_LOG("power: compute quiesced through upstream KFD suspend: %u processes, %u queues unmapped",
                    report.processes, report.queues);
    power_set(MLG_POWER_SUSPENDED, cause, 0);
}

// Upstream KFD resume (rt_power_resume -> kgd2kfd_resume).
static void power_resume(MacLinuxGPU *driver, uint32_t cause)
{
    power_set(MLG_POWER_RESUMING, cause, 0);
    if ((s_power.flags & MLG_POWER_FLAG_KFD_QUIESCED) && !power_session_open()) {
        // The session was quarantined while quiesced: there is nothing to
        // resume into, and an uncertain GPU gets no further work.
        MACLINUXGPU_LOG("power: no session to resume (closing or quarantined)");
        power_set(MLG_POWER_LOST, MLG_POWER_CAUSE_SESSION_CLOSED, 0);
        return;
    }
    if (s_power.flags & MLG_POWER_FLAG_KFD_QUIESCED) {
        if (!power_device_present()) {
            // Gone from the bus while quiesced: never touch its MMIO.
            s_power.flags |= MLG_POWER_FLAG_LINK_DOWN;
            power_fail(driver, MLG_POWER_CAUSE_LINK_DOWN, -19);
            return;
        }
        s_power.flags &= ~MLG_POWER_FLAG_KFD_QUIESCED;
        struct rt_power_report report = {};
        const int r = rt_power_resume(power_pdev(), &report);
        if (r) {
            power_fail(driver, MLG_POWER_CAUSE_RESUME_FAILED, r);
            return;
        }
        MACLINUXGPU_LOG("power: compute resumed through upstream KFD resume: %u queues mapped again",
                        report.active);
    }
    power_set(MLG_POWER_ACTIVE, cause, 0);
}

static void power_run(MacLinuxGPU *driver, enum mlg_power_action action, uint32_t cause)
{
    switch (action) {
    case MLG_POWER_DO_QUIESCE: power_quiesce(driver, cause); break;
    case MLG_POWER_DO_RESUME: power_resume(driver, cause); break;
    case MLG_POWER_DO_IDLE: power_idle(cause); break;
    case MLG_POWER_DO_WAKE_IDLE: power_wake_idle(cause); break;
    default: break;
    }
}

// Upstream removal suspends KFD itself (amdgpu_device_ip_fini_early) and
// kgd2kfd_device_exit gives that suspend back; a suspend this driver still
// holds goes back first, so upstream's count is balanced for the next
// session. Every KFD process has exited by now (dext_compute_stop tore them
// down while their queues were off MES), so nothing is restored.
static void power_before_removal()
{
    if (!(s_power.flags & MLG_POWER_FLAG_KFD_QUIESCED)) return;
    s_power.flags &= ~MLG_POWER_FLAG_KFD_QUIESCED;
    const int r = rt_power_resume(power_pdev(), nullptr);
    MACLINUXGPU_LOG("power: KFD suspend handed back before upstream removal (%d)", r);
}

static void power_device_removed()
{
    s_power.flags |= MLG_POWER_FLAG_LINK_DOWN;
    power_set(MLG_POWER_LOST, MLG_POWER_CAUSE_DEVICE_REMOVED, -19 /* ENODEV */);
}

static bool power_hold(uint64_t client, bool take)
{
    for (auto &holder : s_powerHolders) {
        if (holder != client) continue;
        if (!take) {
            holder = 0;
            --s_power.holds;
        }
        return !take;
    }
    if (!take) return false;
    for (auto &holder : s_powerHolders) {
        if (holder) continue;
        holder = client;
        ++s_power.holds;
        return true;
    }
    return false;
}

static void power_client_prepare(MacLinuxGPU *driver, uint64_t client)
{
    if (!power_hold(client, true)) return;
    MACLINUXGPU_LOG("power: client %llu asks for low power (%u holding)", client, s_power.holds);
    power_run(driver, mlg_power_plan_hold(&s_power, power_session_open()), MLG_POWER_CAUSE_CLIENT_PREPARE);
}

static void power_client_release(MacLinuxGPU *driver, uint64_t client, uint32_t cause)
{
    if (!power_hold(client, false)) return;
    MACLINUXGPU_LOG("power: client %llu %s its low-power hold (%u holding)", client,
                    cause == MLG_POWER_CAUSE_CLIENT_EXIT ? "closed with" : "drops", s_power.holds);
    power_run(driver, mlg_power_plan_release(&s_power), cause);
}

// A new session after LOST: the state is active again.
static void power_session_started()
{
    if (s_power.state != MLG_POWER_LOST) return;
    s_power.flags |= MLG_POWER_FLAG_VRAM_PRESERVED;
    s_power.flags &= ~MLG_POWER_FLAG_LINK_DOWN;
    power_set(MLG_POWER_ACTIVE, MLG_POWER_CAUSE_REPROBED, 0);
}

// The acknowledgement of a deferred power change, exactly once.
static void power_ack(MacLinuxGPU *driver, uint64_t serial, const char *how)
{
    uint64_t expected = serial;
    if (!__atomic_compare_exchange_n(&s_powerAckPending, &expected, 0, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    MACLINUXGPU_LOG("power: SetPowerState(%#x) acknowledged %s", s_powerAckFlags, how);
    driver->AckPowerState(s_powerAckFlags);
}

// On the default queue: whether the session close a sleep waits for ended.
static void power_check_close(MacLinuxGPU *driver)
{
    const uint64_t serial = __atomic_load_n(&s_powerAckPending, __ATOMIC_ACQUIRE);
    if (!serial || !s_powerAckOnClose) return;
    const bool closed = !s_sessionClosing && !s_pciOpen;
    const bool quarantined = s_dmaQuarantined && (s_finalCleanup || s_irqDrainFailed);
    if (!closed && !quarantined) return;
    if (s_power.state == MLG_POWER_SUSPENDING)
        power_set(MLG_POWER_SUSPENDED, MLG_POWER_CAUSE_SYSTEM_SLEEP, quarantined ? -16 : 0);
    power_ack(driver, serial, quarantined ? "with the session quarantined"
                                          : "after the session closed");
}

// The deadline: the change is acknowledged whatever the close is doing,
// so a stuck close never stalls the host's sleep.
static void power_watch(MacLinuxGPU *driver, uint64_t serial)
{
    driver->retain();
    s_powerQueue->DispatchAsync(^{
        const uint64_t deadline = power_now_ns() + uint64_t(MLG_POWER_ACK_DEADLINE_MS) * 1000000;
        while (__atomic_load_n(&s_powerAckPending, __ATOMIC_ACQUIRE) == serial) {
            if (power_now_ns() >= deadline) {
                power_ack(driver, serial, "at its deadline, with the session close still running");
                break;
            }
            if (!__atomic_exchange_n(&s_powerCheckQueued, 1u, __ATOMIC_ACQ_REL)) {
                driver->retain();
                s_bringupQueue->DispatchAsync(^{
                    __atomic_store_n(&s_powerCheckQueued, 0u, __ATOMIC_RELEASE);
                    power_check_close(driver);
                    driver->release();
                });
            }
            IOSleep(10);
        }
        driver->release();
    });
}

// A power change is acknowledged once the driver is safe for it, from the
// default queue after SetPowerState returned (the kernel's call is not
// held while upstream works), or at the deadline. Returns the serial the
// acknowledgement goes by; 0 without a watcher queue (acknowledge at once).
static uint64_t power_defer(MacLinuxGPU *driver, uint32_t powerFlags, bool onClose)
{
    if (!s_powerQueue) return 0;
    s_powerAckFlags = powerFlags;
    s_powerAckOnClose = onClose;
    const uint64_t serial = ++s_powerAckSerial;
    __atomic_store_n(&s_powerAckPending, serial, __ATOMIC_RELEASE);
    power_watch(driver, serial);
    return serial;
}

// SetPowerState(Off): the host is going to sleep. Returns true when the
// acknowledgement waits for a session close (this one, or one already
// running for another reason).
static bool power_sleep(MacLinuxGPU *driver, uint32_t powerFlags)
{
    s_power.flags |= MLG_POWER_FLAG_SYSTEM_SLEEP;
    const enum mlg_power_action action =
        mlg_power_plan_capability(&s_power, MLG_POWER_CAPABILITY_OFF, power_session_open());
    if (action != MLG_POWER_DO_CLOSE_SESSION) {
        power_run(driver, action, MLG_POWER_CAUSE_SYSTEM_SLEEP);
        if (!s_sessionClosing || s_dmaQuarantined) return false;
        MACLINUXGPU_LOG("power: host sleep: waiting for the session close in progress");
        return power_defer(driver, powerFlags, true) != 0;
    }
    // No new GPU work from here; the close runs after this call returns. A
    // quiesced session closes as it is: its queues are already off MES and
    // power_before_removal hands the suspend back.
    power_set(MLG_POWER_SUSPENDING, MLG_POWER_CAUSE_SYSTEM_SLEEP, 0);
    s_power.flags |= MLG_POWER_FLAG_SESSION_CLOSED;
    s_power.flags &= ~MLG_POWER_FLAG_VRAM_PRESERVED;
    MACLINUXGPU_LOG("power: host sleep: closing the compute session before acknowledging "
                    "(VRAM does not survive the link going down)");
    if (!power_defer(driver, powerFlags, true)) {
        close_session(driver);
        return false;
    }
    driver->retain();
    s_bringupQueue->DispatchAsync(^{
        if (s_pciOpen && !s_sessionClosing) close_session(driver);
        power_check_close(driver);
        driver->release();
    });
    return true;
}

// SetPowerState(On) or (Low) with upstream work to do: done after the call
// returns, then acknowledged. Returns true when the acknowledgement waits.
static bool power_device_change(MacLinuxGPU *driver, uint32_t powerFlags, uint32_t capability)
{
    if (capability == MLG_POWER_CAPABILITY_ON)
        s_power.flags &= ~(MLG_POWER_FLAG_SYSTEM_SLEEP | MLG_POWER_FLAG_DEVICE_LOW);
    else
        s_power.flags |= MLG_POWER_FLAG_DEVICE_LOW;
    const enum mlg_power_action action =
        mlg_power_plan_capability(&s_power, capability, power_session_open());
    if (action == MLG_POWER_DO_WAKE_LOST) {
        // The session was closed for the sleep: device memory is gone.
        s_power.flags &= ~MLG_POWER_FLAG_SESSION_CLOSED;
        if (!power_device_present()) s_power.flags |= MLG_POWER_FLAG_LINK_DOWN;
        power_set(MLG_POWER_LOST, MLG_POWER_CAUSE_SYSTEM_WAKE, 0);
        return false;
    }
    const uint32_t cause = capability == MLG_POWER_CAPABILITY_ON ? MLG_POWER_CAUSE_DEVICE_ON
                                                                 : MLG_POWER_CAUSE_DEVICE_LOW;
    if (action != MLG_POWER_DO_QUIESCE && action != MLG_POWER_DO_RESUME) {
        power_run(driver, action, cause); // no upstream call
        return false;
    }
    const uint64_t serial = power_defer(driver, powerFlags, false);
    if (!serial) {
        power_run(driver, action, cause);
        return false;
    }
    driver->retain();
    s_bringupQueue->DispatchAsync(^{
        power_run(driver, mlg_power_plan_capability(&s_power, capability, power_session_open()), cause);
        power_ack(driver, serial, capability == MLG_POWER_CAPABILITY_ON ? "after the resume"
                                                                       : "after the quiesce");
        driver->release();
    });
    return true;
}

// Stopped display clients still to clean up (display_stops_kick).
static IOLock *s_displayStopsLock;

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
    if (!s_displayStopsLock) s_displayStopsLock = IOLockAlloc();
    if (!s_displayStopsLock) return kIOReturnNoMemory;
    {
        uint64_t build[4] = {0, 0, 0, 0};
        (void)dext_compute_runtime_build_cached(build);
        MACLINUXGPU_EVENT("driver start (build %llu)", build[3]);
    }

    // Optional embedded fallback; may be empty and is never assumed complete.
    int fw_result = fw_table_register_embedded();
    if (fw_result != 0) {
        MACLINUXGPU_EVENT("firmware registration failed: %d", fw_result);
        return kIOReturnNoMemory;
    }

    IODispatchQueue *bqueue = nullptr;
    kern_return_t qret = IODispatchQueue::Create("MacLinuxGPUBringup", 0, 0, &bqueue);
    if (qret != kIOReturnSuccess || bqueue == nullptr) {
        MACLINUXGPU_EVENT("IODispatchQueue::Create failed: %#x", qret);
        return qret != kIOReturnSuccess ? qret : kIOReturnNoMemory;
    }
    s_bringupQueue = bqueue;
    qret = SetDispatchQueue(kIOServiceDefaultQueueName, bqueue);
    if (qret != kIOReturnSuccess) {
        bqueue->release();
        s_bringupQueue = nullptr;
        return qret;
    }
    // Device power starts active; a sleep's acknowledgement deadline runs
    // on a queue of its own.
    mlg_power_init(&s_power);
    memset(s_powerHolders, 0, sizeof(s_powerHolders));
    s_powerStartNs = 0;
    __atomic_store_n(&s_powerAckPending, 0, __ATOMIC_RELEASE);
    if (!s_powerQueue && IODispatchQueue::Create("MacLinuxGPUPower", 0, 0, &s_powerQueue) != kIOReturnSuccess) {
        s_powerQueue = nullptr;
        MACLINUXGPU_LOG("power: no queue for sleep acknowledgements; a sleep will not wait for the session close");
    }

    // Compute sessions become KFD processes when the device supports them;
    // a personality may turn that off ("MacLinuxGPUKFDSessions" = false).
    // Display (upstream amdgpu_dm/DC) is off unless a personality sets
    // "MacLinuxGPUDisplay" = true.
    {
        OSDictionary *properties = nullptr;
        bool kfdSessions = true;
        bool display = false;
        if (CopyProperties(&properties) == kIOReturnSuccess && properties) {
            if (properties->getObject("MacLinuxGPUKFDSessions") == kOSBooleanFalse)
                kfdSessions = false;
            if (properties->getObject("MacLinuxGPUDisplay") == kOSBooleanTrue)
                display = true;
            properties->release();
        }
        dext_compute_set_kfd_policy(kfdSessions);
        MACLINUXGPU_LOG("KFD compute sessions %s", kfdSessions ? "enabled when supported" : "disabled");
        const int displayRet = linuxu_driver_set_display(display ? 1 : 0);
        MACLINUXGPU_LOG("display %s%s", display ? "requested (amdgpu.dc=-1)" : "off (amdgpu.dc=0)",
                        displayRet ? " - modules already running, unchanged" : "");
    }

    pci->retain();
    s_retainedPCI = pci;
    s_driver = this;
    s_stopping = false;
    s_retiring = s_retireTerminate = s_terminateRequested = false;
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
    // System Information reads the GPU's name from this service.
    memset(&s_identity, 0, sizeof(s_identity));
    s_displaysPublished = false;
    identity_read_provider(pci);
    identity_publish(this, "start");
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
    // Termination (an upgrade's Retire, a deactivation, an unplug) of a
    // driver with no session: nothing holds the provider, so release it now
    // and let the process exit. Clients were stopped first.
    if (session_idle()) {
        observer_reads_close();
        MACLINUXGPU_LOG("stop: no session; provider released at once");
        FinishStop(provider);
        return kIOReturnSuccess;
    }
    s_stopProvider = provider;
    // An unplug terminates the provider: see whether the device is gone
    // before anything else touches it.
    const bool alreadyClosing = s_sessionClosing;
    if (device_removed("provider stop") && alreadyClosing && s_dmaQuarantined) {
        release_removed(this);
        // Released (and the provider with it, complete_session_close): the
        // session is over. Closing again would find a new, empty session,
        // keep the DMA backing the removal left and quarantine it, and the
        // instance would never exit for a replug.
        if (!s_sessionClosing) return kIOReturnSuccess;
    }
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
    MACLINUXGPU_LOG("session close: final cleanup entered (prepared=%d quarantine=%d removed=%d)",
        s_dmaShutdownPrepared, s_dmaQuarantined, s_deviceRemoved);
    if (s_deviceRemoved && s_dmaQuarantined) {
        release_removed(this);
        return;
    }
    // Compute/upstream producers have stopped and IRQ actions are drained.
    // Release final device-managed aliases under the DMA hold before FLR.
    if (s_rtDevice != nullptr && !s_dmaQuarantined) {
        rt_device_free(s_rtDevice);
        s_rtDevice = nullptr;
    }
    if (s_deviceRemoved && !s_dmaQuarantined) {
        // No reset or isolation of a device that is gone: drop the BAR0
        // aperture mapping and complete what DMA was still retired.
        const int aliases = dext_bar0_cpu_release_orphaned();
        if (aliases > 0)
            MACLINUXGPU_LOG("removal: released %d BAR0 CPU mapping reference(s)", aliases);
        (void)dext_dma_device_removed();
    } else if (s_dmaShutdownPrepared && !s_dmaQuarantined) {
        // With every Linux owner gone, the only BAR0 CPU mapping left is the
        // aperture upstream does not unmap after drm_dev_unplug().
        const int aliases = dext_bar0_cpu_release_orphaned();
        if (aliases > 0)
            MACLINUXGPU_LOG("session close: released %d orphaned BAR0 CPU mapping reference(s) after upstream removal", aliases);
        const int isolated = dext_pci_shutdown_reset();
        if (isolated != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_ENDPOINT_ISOLATION, isolated);
            MACLINUXGPU_EVENT("session close: endpoint isolation failed (%d); DMA backing retained", isolated);
        }
    }
    dext_compute_set_pci_open(false);
    dext_compute_set_stage(DEXT_COMPUTE_STAGE_NONE);
    if (!s_dmaQuarantined) {
        const int released = dext_dma_fini();
        if (released != 0 && s_deviceRemoved) {
            // Nothing on the bus can use what is left: keep the backing,
            // but never the provider of a device that is gone.
            MACLINUXGPU_EVENT("removal: DMA backing still owned (%d) is kept; the provider closes", released);
        } else if (released != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_DMA_RETAINED, released);
            MACLINUXGPU_LOG("session close: live DMA backing retained (%d)", released);
        }
    }
    if (s_dmaQuarantined) {
        quarantine_session(this);
        MACLINUXGPU_EVENT("session quarantined: retaining clients, provider and runtime owners");
        // Superclass Stop invalidates the provider. Pending Stop requests
        // retain their objects until quiescence can actually be established;
        // a stopping driver releases at once when cached state proves it.
        if (s_stopping) (void)release_quarantine(this);
        return;
    }
    MACLINUXGPU_EVENT("session closed after upstream removal, interrupt drain and endpoint isolation");
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

// DriverKit delivers the provider's power changes here (IOService.iig):
// kIOServicePowerCapabilityOff before system sleep, On when the device and
// system are fully powered again, Low for a reduced device power state
// while the system runs. The change is acknowledged by passing it to the
// superclass, after the driver made itself safe for it (power_state.h).
kern_return_t
IMPL(MacLinuxGPU, SetPowerState)
{
    if (s_driver != this) return SetPowerState(powerFlags, SUPERDISPATCH);
    MACLINUXGPU_LOG("power: SetPowerState(%#x) in state %s, session %s", powerFlags,
                    power_state_name(s_power.state), power_session_open() ? "open" : "none");
    // A change that needs work is acknowledged later (AckPowerState).
    if (powerFlags == kIOServicePowerCapabilityOff) {
        if (power_sleep(this, powerFlags)) return kIOReturnSuccess;
    } else if (powerFlags & kIOServicePowerCapabilityOn) {
        if (power_device_change(this, powerFlags, MLG_POWER_CAPABILITY_ON)) return kIOReturnSuccess;
    } else if (powerFlags & kIOServicePowerCapabilityLow) {
        if (power_device_change(this, powerFlags, MLG_POWER_CAPABILITY_LOW)) return kIOReturnSuccess;
    }
    return SetPowerState(powerFlags, SUPERDISPATCH);
}

void
MacLinuxGPU::AckPowerState(uint32_t powerFlags)
{
    (void)SetPowerState(powerFlags, SUPERDISPATCH);
}

kern_return_t
IMPL(MacLinuxGPU, NewUserClient)
{
    // Observers read cached state only, so they may attach while a session
    // closes or stays quarantined; session clients still may not.
    const bool observer = type == MLG_USER_CLIENT_OBSERVER;
    const bool linuxFile = type == MLG_USER_CLIENT_LINUX_FILE;
    if (s_stopping || s_driver != this || ((s_sessionClosing || s_retiring) && !observer))
        return kIOReturnNotAttached;
    if (type != MLG_USER_CLIENT_SESSION && !observer && !linuxFile) {
        MACLINUXGPU_LOG("unsupported user-client type %u", (unsigned)type);
        return kIOReturnUnsupported;
    }
    // The codegen'd UserClient class (from MacLinuxGPUUserClient.iig) is
    // OSMetaClass-registered; Create instantiates it (the iig codegen's
    // registry), not `new`.  This is the ONE thing the make build stubs
    // (it returns kIOReturnUnsupported).
    IOService *clientService = nullptr;
    s_creatingObserver = observer;
    s_creatingLinuxFile = linuxFile;
    kern_return_t ret = Create(this, "MacLinuxGPUUserClientProperties",
                               &clientService);
    s_creatingObserver = false;
    s_creatingLinuxFile = false;
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
    if (created && created->ivars) {
        created->ivars->observer = observer;
        created->ivars->linuxFile = linuxFile;
    } else if ((observer && !pending_role_add(s_pendingObservers, clientService)) ||
               (linuxFile && !pending_role_add(s_pendingLinuxFiles, clientService))) {
        clientService->release();
        MACLINUXGPU_LOG("too many clients starting at once");
        return kIOReturnNoResources;
    }
    *userClient = typed;
    MACLINUXGPU_LOG("NewUserClient: MacLinuxGPUUserClient created%s",
                    observer ? " (observer)" : linuxFile ? " (Linux file)" : "");
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
    if (s_powerQueue != nullptr) {
        s_powerQueue->release();
        s_powerQueue = nullptr;
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

// A client's display result slot (defined with the display calls).
static DisplayResultSlot *display_slot_new();
static void display_slot_put(DisplayResultSlot *slot);

kern_return_t
IMPL(MacLinuxGPUUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    MacLinuxGPU *driver = OSDynamicCast(MacLinuxGPU, provider);
    if (driver == nullptr) return kIOReturnUnsupported;
    const bool observer = s_creatingObserver || pending_role_take(s_pendingObservers, this);
    const bool linuxFile = !observer &&
        (s_creatingLinuxFile || pending_role_take(s_pendingLinuxFiles, this));
    if (s_driver != driver || s_stopping || ((s_sessionClosing || s_retiring) && !observer))
        return kIOReturnNotAttached;
    IODispatchQueue *ownerQueue = nullptr;
    ret = driver->CopyDispatchQueue(kIOServiceDefaultQueueName, &ownerQueue);
    if (ret != kIOReturnSuccess || ownerQueue == nullptr)
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    // Every client gets a queue of its own for its calls. DriverKit runs a
    // call on its one delivery thread, under the target queue: a call that
    // waited for the owner's queue (busy with a probe, a session close, or
    // a client's session call that sleeps) would hold every client and the
    // driver's Stop with it. Session work runs on the owner's queue as
    // async calls (owner_call), in arrival order.
    IODispatchQueue *clientQueue = nullptr;
    ret = IODispatchQueue::Create(observer ? "MacLinuxGPUObserver" :
                                  linuxFile ? "MacLinuxGPULinuxFile" : "MacLinuxGPUSession",
                                  0, 0, &clientQueue);
    if (ret != kIOReturnSuccess || clientQueue == nullptr) {
        ownerQueue->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = SetDispatchQueue(kIOServiceDefaultQueueName, clientQueue);
    clientQueue->release();
    if (ret != kIOReturnSuccess || s_nextClientID == UINT64_MAX) {
        ownerQueue->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }
    ivars = IONewZero(MacLinuxGPUUserClient_IVars, 1);
    if (!ivars) { ownerQueue->release(); return kIOReturnNoMemory; }
    ivars->displayResults = display_slot_new();
    ivars->ownerLock = IOLockAlloc();
    if (!ivars->displayResults || !ivars->ownerLock) {
        display_slot_put(ivars->displayResults);
        if (ivars->ownerLock) IOLockFree(ivars->ownerLock);
        IOSafeDeleteNULL(ivars, MacLinuxGPUUserClient_IVars, 1);
        ownerQueue->release();
        return kIOReturnNoMemory;
    }
    ivars->ownerQueue = ownerQueue;
    driver->retain();
    ivars->ownerDriver = driver;
    ivars->clientID = ++s_nextClientID;
    ivars->observer = observer;
    ivars->linuxFile = linuxFile;
    MACLINUXGPU_LOG("UserClient Start (client %llu, type %u)", ivars->clientID,
                    observer ? MLG_USER_CLIENT_OBSERVER :
                    linuxFile ? MLG_USER_CLIENT_LINUX_FILE : MLG_USER_CLIENT_SESSION);
    return kIOReturnSuccess;
}

// A Linux-file client's session membership ends on the owner's queue, as a
// session client's does (it holds no compute handles to release).
static void lx_finish_stop(MacLinuxGPUUserClient *client, IOService *provider)
{
    auto *iv = client->ivars;
    // A first call that joined on this queue after the Stop took the
    // client's process (lx_join_main ran in between): that process exits
    // too, on a thread of its own.
    if (struct rt_lx_client *late = lx_take(client)) (void)rt_lx_client_retire(late, nullptr, nullptr);
    const bool participant = iv->sessionGeneration == s_sessionGeneration;
    if (participant) {
        iv->sessionGeneration = 0;
        if (s_participants) --s_participants;
    }
    if (s_sessionClosing || (participant && (!s_participants || s_dmaQuarantined))) {
        iv->nextStopping = s_stoppingClients;
        s_stoppingClients = client;
        close_session(iv->ownerDriver);
    } else {
        client->FinishStop(provider);
    }
}

// The client's process exits (async calls return, files close through
// upstream postclose) on a thread of its own, never on this one: a call of
// the process that does not return (a wait on a GPU that stopped) would
// otherwise hold the incoming-call thread, and with it every client's
// calls and the driver's own Stop. On an unplug this Stop began removal
// first (device_removed), which completes the GPU's fences, so the
// process's calls return. Its session membership ends after it, on the
// owner's queue.
struct LxStop {
    MacLinuxGPUUserClient *client;
    IOService *provider;
};
static void lx_stop_finished(void *arg)
{
    auto *stop = static_cast<LxStop *>(arg);
    MacLinuxGPUUserClient *client = stop->client;
    IOService *provider = stop->provider;
    IOFree(stop, sizeof(*stop));
    s_lxCalls.leave();
    client->ivars->ownerQueue->DispatchAsync(^{ lx_finish_stop(client, provider); });
}

static void lx_client_stop(MacLinuxGPUUserClient *client, IOService *provider)
{
    if (s_lxCalls.enter()) {
        struct rt_lx_client *lx = lx_take(client);
        auto *stop = lx ? static_cast<LxStop *>(IOMallocZero(sizeof(LxStop))) : nullptr;
        if (lx) {
            if (stop) {
                stop->client = client;
                stop->provider = provider;
            }
            // Its exit runs on a thread of its own, or (no thread: logged)
            // at the session's close (rt_lx_retire_drain); never here.
            if (!rt_lx_client_retire(lx, stop ? lx_stop_finished : nullptr, stop)) {
                if (stop) return;  // lx_stop_finished finishes the Stop
            } else if (stop) {
                IOFree(stop, sizeof(*stop));
            }
            if (!stop)
                MACLINUXGPU_LOG("client %llu: no memory to follow its process's exit; "
                                "its Stop finishes now", client->ivars->clientID);
        }
        s_lxCalls.leave();
    }
    // Otherwise a session close owns the teardown (lx_teardown_all).
    client->ivars->ownerQueue->DispatchAsync(^{ lx_finish_stop(client, provider); });
}

static void session_client_stop(MacLinuxGPUUserClient *client, IOService *provider);
static void owner_results_free(MacLinuxGPUUserClient *client);
static IOMemoryDescriptor *client_memory_find(MacLinuxGPUUserClient *client, uint64_t type);

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
        // No session membership or mappings. A display agent's imports and
        // the output it started end with it (on its own queue, so none of
        // its calls is in flight); a closing session has released them.
        // It finishes on the owner's queue, after its session calls.
        observer_display_client_stop(ivars->clientID);
        ivars->ownerQueue->DispatchAsync(^{ FinishStop(provider); });
        return kIOReturnSuccess;
    }
    // An unplug terminates the clients before the provider: see whether the
    // device is gone before this client's teardown waits on it.
    (void)device_removed("client stop");
    if (ivars->linuxFile) {
        lx_client_stop(this, provider);
        return kIOReturnSuccess;
    }
    // The rest is session work: on the owner's queue, after the client's
    // session calls, never on the delivery thread.
    ivars->ownerQueue->DispatchAsync(^{ session_client_stop(this, provider); });
    return kIOReturnSuccess;
}

static void session_client_stop(MacLinuxGPUUserClient *client, IOService *provider)
{
    auto *ivars = client->ivars;
    const bool participant = ivars->sessionGeneration == s_sessionGeneration;
    if (participant && s_participants > 1 && !s_sessionClosing) {
        // IRQ delivery stays active while this client's queues are removed.
        const int released = dext_compute_release_client(ivars->clientID);
        if (released != 0 && s_deviceRemoved) {
            MACLINUXGPU_EVENT("client close after removal: cleanup returned %d; nothing can run", released);
        } else if (released != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_CLIENT_RELEASE, released);
            MACLINUXGPU_EVENT("client close: cleanup failed (%d); retaining uncertain shared-session backing", released);
        }
    }
    if (participant) {
        ivars->sessionGeneration = 0;
        if (s_participants) --s_participants;
    }
    if (s_sessionClosing || (participant && (!s_participants || s_dmaQuarantined))) {
        ivars->nextStopping = s_stoppingClients;
        s_stoppingClients = client;
        close_session(ivars->ownerDriver);
    } else {
        client->FinishStop(provider);
    }
}

void
MacLinuxGPUUserClient::FinishStop(IOService *provider)
{
    MacLinuxGPU *driver = ivars->ownerDriver;
    // Its power waits end now; a low-power hold it kept goes on the
    // default queue, where every power transition runs.
    power_cancel_waits(this);
    if (s_bringupQueue) {
        const uint64_t client = ivars->clientID;
        driver->retain();
        s_bringupQueue->DispatchAsync(^{
            power_client_release(driver, client, MLG_POWER_CAUSE_CLIENT_EXIT);
            driver->release();
        });
    }
    // An observer never holds a lease, and stops on its own queue.
    if (!ivars->observer) s_rawBARLease.release(ivars->clientID);
    if (ivars->ownerQueue) ivars->ownerQueue->release();
    display_slot_put(ivars->displayResults);
    owner_results_free(this);
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

// ----------------------------------------------------------------
// Linux-file calls (rt/lx_abi.h selectors), on the client's own queue.
// ----------------------------------------------------------------

// The client's process, created by its first call, on the owner's queue
// (lx_join): the client joins the session as a session client does, then
// gets a Linux process with its creator's pid and name. The delivery
// thread only reads whether it exists (lx_ready).
static struct rt_lx_client *lx_ready(MacLinuxGPUUserClient *client)
{
    auto *iv = client->ivars;
    lx_registry_acquire();
    struct rt_lx_client *lx = iv->lx &&
        iv->sessionGeneration == __atomic_load_n(&s_sessionGeneration, __ATOMIC_ACQUIRE) ?
        iv->lx : nullptr;
    lx_registry_release();
    return lx;
}

// On the owner's queue.
static kern_return_t lx_join(MacLinuxGPUUserClient *client, struct rt_lx_client **out)
{
    auto *iv = client->ivars;
    if (iv->stopping) return kIOReturnNotAttached;
    if ((*out = lx_ready(client))) return kIOReturnSuccess;
    // A process from a session that has closed: never used since its
    // admission closed, so it holds no files of the driver that is gone;
    // it exits on a thread of its own all the same.
    if (struct rt_lx_client *stale = lx_take(client)) (void)rt_lx_client_retire(stale, nullptr, nullptr);
    if (s_stopping || s_sessionClosing || s_dmaQuarantined || !s_modulesRunning || !s_rtDevice)
        return kIOReturnNotReady;
    kern_return_t ret = ensure_open(client);
    if (ret != kIOReturnSuccess) return ret;
    void *pdev = rt_device_get_pdev(s_rtDevice);
    int pid = 0;
    char name[32];
    client_creator(client, &pid, name, sizeof(name));
    struct rt_lx_client *lx = nullptr;
    if (rt_lx_client_create(static_cast<struct pci_dev *>(pdev), pid, name[0] ? name : nullptr, &lx))
        return kIOReturnNoMemory;
    // The primary node and LX_SCANOUT go through the display output.
    rt_lx_client_set_display(lx, &rt_display_lx_hooks);
    lx_registry_acquire();
    iv->lx = lx;
    iv->nextLinuxFile = s_linuxFiles;
    s_linuxFiles = client;
    lx_registry_release();
    MACLINUXGPU_LOG("client %llu: Linux process pid %d (%s)", iv->clientID,
                    rt_lx_client_pid(lx), name[0] ? name : "unnamed");
    *out = lx;
    return kIOReturnSuccess;
}

// A call that did not reach the process.
static kern_return_t lx_transport_error(int r)
{
    switch (r) {
    case -MLG_LX_ENOMEM: return kIOReturnNoMemory;
    case -MLG_LX_ENOSPC: return kIOReturnNoSpace;
    case -MLG_LX_ESRCH: return kIOReturnNotAttached;
    case -MLG_LX_EAGAIN: return kIOReturnBusy;
    case -MLG_LX_EDEADLK: return kIOReturnNotPermitted;  // can sleep: async only
    case -MLG_LX_EINVAL:
    case -MLG_LX_E2BIG:
    case -MLG_LX_EFAULT: return kIOReturnBadArgument;
    default: return kIOReturnError;
    }
}

// The request frame: inline structure input, or a descriptor above 4096 bytes.
struct LxInput {
    const void *bytes = nullptr;
    size_t length = 0;
    IOMemoryMap *map = nullptr;
    ~LxInput() { if (map) map->release(); }
};
static bool lx_input(IOUserClientMethodArguments *a, LxInput &in)
{
    if (a->structureInput) {
        in.bytes = a->structureInput->getBytesNoCopy();
        in.length = a->structureInput->getLength();
    } else if (a->structureInputDescriptor) {
        if (a->structureInputDescriptor->CreateMapping(kIOMemoryMapReadOnly, 0, 0, 0, 0,
                                                       &in.map) != kIOReturnSuccess || !in.map)
            return false;
        in.bytes = reinterpret_cast<const void *>(in.map->GetAddress());
        in.length = (size_t)in.map->GetLength();
    }
    return in.bytes && in.length;
}

// Where the reply frame goes: the caller's output descriptor when it gave
// one (above 4096 bytes), else structure output.
struct LxOutput {
    void *bytes = nullptr;
    size_t capacity = 0;
    IOMemoryMap *map = nullptr;
    bool heap = false;
    ~LxOutput() {
        if (map) map->release();
        if (heap) IOFree(bytes, capacity);
    }
};
static bool lx_output(IOUserClientMethodArguments *a, LxOutput &out)
{
    if (a->structureOutputDescriptor) {
        if (a->structureOutputDescriptor->CreateMapping(0, 0, 0, 0, 0, &out.map) != kIOReturnSuccess ||
            !out.map)
            return false;
        out.bytes = reinterpret_cast<void *>(out.map->GetAddress());
        out.capacity = (size_t)out.map->GetLength();
        return out.bytes != nullptr;
    }
    out.capacity = a->structureOutputMaximumSize < MLG_LX_INLINE_STRUCT_BYTES ?
        (size_t)a->structureOutputMaximumSize : MLG_LX_INLINE_STRUCT_BYTES;
    if (!out.capacity) return false;
    out.bytes = IOMalloc(out.capacity);
    out.heap = out.bytes != nullptr;
    return out.heap;
}
static kern_return_t lx_output_done(IOUserClientMethodArguments *a, LxOutput &out, size_t length)
{
    if (out.map) return kIOReturnSuccess;
    a->structureOutput = OSData::withBytes(out.bytes, length);
    return a->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

// An async call's completion: async data [0] token, [1] result, [2] reply
// bytes, then the reply when it fits.
struct LxAsync {
    MacLinuxGPUUserClient *client;
    OSAction *action;
};
static void lx_async_done(void *ctx, uint64_t token, int64_t result, const void *rbuf,
                          size_t reply_bytes)
{
    auto *a = static_cast<LxAsync *>(ctx);
    IOUserClientAsyncArgumentsArray data = {};
    uint32_t count = 3;
    data[0] = token;
    data[1] = (uint64_t)result;
    data[2] = reply_bytes;
    if (rbuf && reply_bytes && reply_bytes <= MLG_LX_ASYNC_INLINE_BYTES) {
        memcpy(&data[3], rbuf, reply_bytes);
        count += (uint32_t)((reply_bytes + 7) / 8);
    }
    a->client->AsyncCompletion(a->action, kIOReturnSuccess, data, count);
    a->action->release();
    a->client->release();
    IOFree(a, sizeof(*a));
}

// An async call's context: the client and its completion, retained until
// the worker completes it (it may finish after the client stopped).
static LxAsync *lx_async_begin(MacLinuxGPUUserClient *client, OSAction *action)
{
    auto *ctx = static_cast<LxAsync *>(IOMallocZero(sizeof(LxAsync)));
    if (!ctx) return nullptr;
    ctx->client = client;
    ctx->action = action;
    client->retain();
    action->retain();
    return ctx;
}
static void lx_async_abandon(LxAsync *ctx)
{
    ctx->action->release();
    ctx->client->release();
    IOFree(ctx, sizeof(*ctx));
}

// Interrupt-driven waits (selectors 86 and 87, session_state.h).
// dext_compute's linuxu error codes as the Linux errno the client sees.
static int event_errno(int r)
{
    switch (r) {
    case 0: return 0;
    case -ENOENT_L: return -2;    /* ENOENT */
    case -ENOMEM_L: return -12;   /* ENOMEM */
    case -EBUSY_L: return -16;    /* EBUSY */
    case -EINVAL_L: return -22;   /* EINVAL */
    default: return -19;          /* ENODEV: no KFD process, or not ready */
    }
}

struct EventWaitJob {
    MacLinuxGPUUserClient *client;
    OSAction *action;
    struct rt_kfd_wait *wait;
    uint64_t token;
};

// On a wait-pool thread: asleep in WAIT_EVENTS until KFD's interrupt
// handler signals an event or the timeout passes, then the completion.
static void event_wait_main(void *arg)
{
    auto *job = static_cast<EventWaitJob *>(arg);
    uint32_t result = 2;
    const int r = rt_kfd_wait_run(job->wait, &result);
    IOUserClientAsyncArgumentsArray data = {};
    data[0] = job->token;
    data[1] = (uint64_t)(int64_t)r;
    data[2] = result;
    job->client->AsyncCompletion(job->action, kIOReturnSuccess, data, MLG_EVENT_WAIT_WORDS);
    job->action->release();
    job->client->release();
    IOFree(job, sizeof(*job));
}

static kern_return_t lx_call(MacLinuxGPUUserClient *client, struct rt_lx_client *lx,
                             uint64_t selector, IOUserClientMethodArguments *a,
                             uint64_t entered_ns, uint64_t admitted_ns)
{
    const uint64_t *in = a->scalarInput;
    uint64_t *out = a->scalarOutput;
    const uint32_t nin = a->scalarInputCount;
    if (!in || !out) return kIOReturnBadArgument;
    switch (selector) {
    case MLG_SELECTOR_LX_RETIRED_OPEN:
    case MLG_SELECTOR_LX_RETIRED_CLOSE:
    case MLG_SELECTOR_LX_RETIRED_MMAP:
    case MLG_SELECTOR_LX_RETIRED_MUNMAP:
        MACLINUXGPU_LOG("client %llu: selector %llu (a synchronous open, close, mmap or munmap) "
                        "is retired: those can sleep and go through LX_CALL_ASYNC; the client "
                        "library is older than the driver and must be rebuilt",
                        client->ivars->clientID, selector);
        return kIOReturnUnsupported;
    case MLG_SELECTOR_LX_CALL_ASYNC: {
        if (!a->completion || nin < 1 || nin > MLG_LX_OP_MMAP_ARGS || a->scalarOutputCount < 2)
            return kIOReturnBadArgument;
        LxAsync *ctx = lx_async_begin(client, a->completion);
        if (!ctx) return kIOReturnNoMemory;
        uint64_t token = 0;
        const int r = rt_lx_op_async(lx, in, nin, lx_async_done, ctx, &token);
        if (r) lx_async_abandon(ctx);
        out[0] = (uint64_t)(int64_t)r;  // not started: nothing will complete
        out[1] = token;
        a->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }
    case MLG_SELECTOR_LX_IOCTL:
    case MLG_SELECTOR_LX_IOCTL_ASYNC: {
        if (nin != 2 || a->scalarOutputCount < 2 || in[0] > INT32_MAX || in[1] > UINT32_MAX)
            return kIOReturnBadArgument;
        LxInput frame;
        if (!lx_input(a, frame)) return kIOReturnBadArgument;
        if (selector == MLG_SELECTOR_LX_IOCTL_ASYNC) {
            if (!a->completion) return kIOReturnBadArgument;
            LxAsync *ctx = lx_async_begin(client, a->completion);
            if (!ctx) return kIOReturnNoMemory;
            uint64_t token = 0;
            const int r = rt_lx_ioctl_async(lx, (int)in[0], (uint32_t)in[1], frame.bytes,
                                            frame.length, lx_async_done, ctx, &token);
            if (r) lx_async_abandon(ctx);
            out[0] = (uint64_t)(int64_t)r;  // not started: nothing will complete
            out[1] = token;
            a->scalarOutputCount = 2;
            return kIOReturnSuccess;
        }
        LxOutput reply;
        if (!lx_output(a, reply)) return kIOReturnBadArgument;
        const uint32_t cmd = (uint32_t)in[1];
        const uint64_t args_ns = rt_lx_time_ns();
        rt_lx_timing_add(cmd, RT_LX_HOP_ADMIT, admitted_ns - entered_ns);
        rt_lx_timing_add(cmd, RT_LX_HOP_ARGS, args_ns - admitted_ns);
        size_t bytes = 0;
        int64_t result = 0;
        // This thread is every client's: only requests that cannot sleep
        // run on it (rt_lx_ioctl_nosleep refuses the rest, logged).
        const int r = rt_lx_ioctl_nosleep(lx, (int)in[0], cmd, frame.bytes, frame.length,
                                          reply.bytes, reply.capacity, &bytes, &result);
        if (r) return lx_transport_error(r);
        const uint64_t ran_ns = rt_lx_time_ns();
        out[0] = (uint64_t)result;
        out[1] = bytes;
        a->scalarOutputCount = 2;
        const kern_return_t ret = lx_output_done(a, reply, bytes);
        const uint64_t done_ns = rt_lx_time_ns();
        rt_lx_timing_add(cmd, RT_LX_HOP_REPLY, done_ns - ran_ns);
        rt_lx_timing_add(cmd, RT_LX_HOP_TOTAL, done_ns - entered_ns);
        return ret;
    }
    case MLG_SELECTOR_LX_RESULT: {
        if (nin != 1 || a->scalarOutputCount < 2) return kIOReturnBadArgument;
        LxOutput reply;
        if (!lx_output(a, reply)) return kIOReturnBadArgument;
        size_t bytes = 0;
        int64_t result = 0;
        const int r = rt_lx_result(lx, in[0], reply.bytes, reply.capacity, &bytes, &result);
        if (r == -MLG_LX_ENOENT) return kIOReturnBadArgument;
        if (r == -MLG_LX_EBUSY) return kIOReturnBusy;
        if (r) return lx_transport_error(r);
        out[0] = (uint64_t)result;
        out[1] = bytes;
        a->scalarOutputCount = 2;
        return lx_output_done(a, reply, bytes);
    }
    case MLG_SELECTOR_LX_MMAP_COMMIT:
        if (nin != 2 || a->scalarOutputCount < 1) return kIOReturnBadArgument;
        out[0] = (uint64_t)(int64_t)rt_lx_mmap_commit(lx, in[0], in[1]);
        a->scalarOutputCount = 1;
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

// LX_SCANOUT: structure in, scalar result and structure out (rt/lx_abi.h).
static kern_return_t lx_scanout_call(struct rt_lx_client *lx, IOUserClientMethodArguments *a)
{
    if (!a->scalarOutput || a->scalarOutputCount < 1 || !a->structureInput ||
        a->structureInput->getLength() != sizeof(struct mlg_lx_scanout) ||
        a->structureOutputMaximumSize < sizeof(struct mlg_lx_scanout_state))
        return kIOReturnBadArgument;
    struct mlg_lx_scanout req;
    memcpy(&req, a->structureInput->getBytesNoCopy(), sizeof(req));
    struct mlg_lx_scanout_state state = {};
    const int r = rt_lx_scanout(lx, &req, &state);
    a->scalarOutput[0] = (uint64_t)(int64_t)r;
    a->scalarOutputCount = 1;
    a->structureOutput = OSData::withBytes(&state, sizeof(state));
    return a->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
}

// A client's first LX_CALL_ASYNC: the join (lx_join) and the operation on
// the owner's queue; the delivery thread returns at once (token 0: the
// completion carries the operation's own).
struct LxJoin {
    MacLinuxGPUUserClient *client;
    LxAsync *ctx;
    uint64_t in[MLG_LX_OP_MMAP_ARGS];
    uint32_t nin;
};
static void lx_join_main(LxJoin *job)
{
    struct rt_lx_client *lx = nullptr;
    const kern_return_t kr = lx_join(job->client, &lx);
    int r = kr == kIOReturnSuccess ? 0 :
            kr == kIOReturnNoMemory ? -MLG_LX_ENOMEM : -MLG_LX_ENODEV;
    if (!r) {
        uint64_t token = 0;
        if (!s_lxCalls.enter()) {
            r = -MLG_LX_ENODEV;
        } else {
            r = rt_lx_op_async(lx, job->in, job->nin, lx_async_done, job->ctx, &token);
            s_lxCalls.leave();
        }
    }
    // Not started: complete it here (the worker completes the others).
    if (r) lx_async_done(job->ctx, 0, r, nullptr, 0);
    IOFree(job, sizeof(*job));
}

static kern_return_t lx_join_call(MacLinuxGPUUserClient *client, IOUserClientMethodArguments *a)
{
    const uint32_t nin = a->scalarInputCount;
    if (!a->completion || !a->scalarInput || nin < 1 || nin > MLG_LX_OP_MMAP_ARGS ||
        !a->scalarOutput || a->scalarOutputCount < 2)
        return kIOReturnBadArgument;
    auto *job = static_cast<LxJoin *>(IOMallocZero(sizeof(LxJoin)));
    if (!job) return kIOReturnNoMemory;
    job->ctx = lx_async_begin(client, a->completion);
    if (!job->ctx) {
        IOFree(job, sizeof(*job));
        return kIOReturnNoMemory;
    }
    job->client = client;
    job->nin = nin;
    memcpy(job->in, a->scalarInput, nin * sizeof(uint64_t));
    client->ivars->ownerQueue->DispatchAsync(^{ lx_join_main(job); });
    a->scalarOutput[0] = 0;
    a->scalarOutput[1] = 0;
    a->scalarOutputCount = 2;
    return kIOReturnSuccess;
}

static kern_return_t lx_external_method(MacLinuxGPUUserClient *client, uint64_t selector,
                                        IOUserClientMethodArguments *arguments)
{
    const uint64_t entered_ns = rt_lx_time_ns();
    struct rt_lx_client *lx = lx_ready(client);
    if (!lx) {
        // No process yet (or one from a closed session): its first call is
        // an open (LX_CALL_ASYNC), which joins on the owner's queue.
        return selector == MLG_SELECTOR_LX_CALL_ASYNC ? lx_join_call(client, arguments)
                                                      : kIOReturnNotReady;
    }
    kern_return_t ret;
    if (!s_lxCalls.enter()) return kIOReturnNotReady;
    ret = selector == MLG_SELECTOR_LX_SCANOUT ? lx_scanout_call(lx, arguments) :
                                                lx_call(client, lx, selector, arguments, entered_ns,
                                                        rt_lx_time_ns());
    s_lxCalls.leave();
    return ret;
}

// A Linux-file mapping as client memory: its page runs, or its BAR ranges.
struct LxRanges {
    uint64_t *addresses, *lengths;
    uint32_t *bars;
    size_t count, capacity;
};
static int lx_collect_range(void *arg, uint32_t backing, uint32_t bar, uint64_t addr, uint64_t bytes)
{
    (void)backing;
    auto *r = static_cast<LxRanges *>(arg);
    if (r->count == r->capacity) return -1;
    r->addresses[r->count] = addr;
    r->lengths[r->count] = bytes;
    r->bars[r->count] = bar;
    ++r->count;
    return 0;
}

// Concatenate @count descriptors (consumed), 32 per level.
static IOMemoryDescriptor *lx_concat(IOMemoryDescriptor **descs, size_t count)
{
    while (count > 1) {
        size_t next = 0;
        for (size_t i = 0; i < count; i += 32) {
            const uint32_t n = (uint32_t)(count - i > 32 ? 32 : count - i);
            IOMemoryDescriptor *parent = nullptr;
            if (IOMemoryDescriptor::CreateWithMemoryDescriptors(kIOMemoryDirectionOutIn, n,
                                                                &descs[i], &parent) != kIOReturnSuccess ||
                !parent) {
                for (size_t j = 0; j < next; ++j) descs[j]->release();
                for (size_t j = i; j < count; ++j) descs[j]->release();
                return nullptr;
            }
            for (size_t j = i; j < i + n; ++j) descs[j]->release();
            descs[next++] = parent;
        }
        count = next;
    }
    return count ? descs[0] : nullptr;
}

static kern_return_t lx_copy_memory(MacLinuxGPUUserClient *client, uint64_t type,
                                    uint64_t *options, IOMemoryDescriptor **memory)
{
    if (type < MLG_LX_MMAP_TYPE_BASE || type > MLG_LX_MMAP_TYPE_LIMIT || !options || !memory)
        return kIOReturnBadArgument;
    if (!s_lxCalls.enter()) return kIOReturnNotReady;
    struct rt_lx_client *lx = client->ivars->lx;
    struct rt_lx_map_info info = {};
    kern_return_t ret = lx && !rt_lx_map_info(lx, type, &info) && info.ranges ?
        kIOReturnSuccess : kIOReturnBadArgument;
    LxRanges ranges = {};
    if (ret == kIOReturnSuccess) {
        ranges.capacity = info.ranges;
        ranges.addresses = static_cast<uint64_t *>(IOMallocZero(ranges.capacity * sizeof(uint64_t)));
        ranges.lengths = static_cast<uint64_t *>(IOMallocZero(ranges.capacity * sizeof(uint64_t)));
        ranges.bars = static_cast<uint32_t *>(IOMallocZero(ranges.capacity * sizeof(uint32_t)));
        if (!ranges.addresses || !ranges.lengths || !ranges.bars ||
            rt_lx_map_ranges(lx, type, lx_collect_range, &ranges) || ranges.count != info.ranges)
            ret = kIOReturnNoMemory;
    }
    IOMemoryDescriptor *descriptor = nullptr;
    if (ret == kIOReturnSuccess && info.backing == RT_LX_RANGE_CPU) {
        // GTT pages and kernel pages: the dext's own buffers.
        descriptor = static_cast<IOMemoryDescriptor *>(
            dext_dma_copy_ranges_descriptor(ranges.addresses, ranges.lengths, ranges.count));
    } else if (ret == kIOReturnSuccess && info.backing == RT_LX_RANGE_BAR && s_retainedPCI) {
        // CPU-visible VRAM and doorbells: ranges of the BARs.
        auto **descs = static_cast<IOMemoryDescriptor **>(
            IOMallocZero(ranges.count * sizeof(IOMemoryDescriptor *)));
        size_t built = 0;
        for (; descs && built < ranges.count; ++built) {
            uint8_t memoryIndex = 0, barType = 0;
            uint64_t barSize = 0;
            IOMemoryDescriptor *bar = nullptr;
            if (ranges.bars[built] > 5 ||
                s_retainedPCI->GetBARInfo((uint8_t)ranges.bars[built], &memoryIndex, &barSize,
                                          &barType) != kIOReturnSuccess ||
                s_retainedPCI->_CopyDeviceMemoryWithIndex(memoryIndex, &bar,
                                                          client->GetProvider()) != kIOReturnSuccess ||
                !bar)
                break;
            const kern_return_t sub = IOMemoryDescriptor::CreateSubMemoryDescriptor(
                kIOMemoryDirectionOutIn, ranges.addresses[built], ranges.lengths[built], bar,
                &descs[built]);
            bar->release();
            if (sub != kIOReturnSuccess || !descs[built]) break;
        }
        if (descs && built == ranges.count) {
            descriptor = lx_concat(descs, built);
            /* A CPU mapping of the GPU's BARs in the client's process: a
             * store through it to an unplugged GPU panics the Mac as the
             * driver's own did, so each one is logged (which clients map
             * what, for that decision). */
            uint64_t bytes = 0;
            for (size_t i = 0; i < ranges.count; ++i) bytes += ranges.lengths[i];
            MACLINUXGPU_LOG("client %llu: maps %llu bytes of BAR%u into its process (%s)",
                            client->ivars->clientID, (unsigned long long)bytes, ranges.bars[0],
                            ranges.bars[0] == 0 ? "VRAM" : "doorbells or registers");
        } else if (descs) {
            for (size_t i = 0; i < built; ++i) descs[i]->release();
        }
        if (descs) IOFree(descs, ranges.count * sizeof(IOMemoryDescriptor *));
    }
    if (ranges.addresses) IOFree(ranges.addresses, ranges.capacity * sizeof(uint64_t));
    if (ranges.lengths) IOFree(ranges.lengths, ranges.capacity * sizeof(uint64_t));
    if (ranges.bars) IOFree(ranges.bars, ranges.capacity * sizeof(uint32_t));
    s_lxCalls.leave();
    if (ret != kIOReturnSuccess) return ret;
    if (!descriptor) return kIOReturnNotReady;
    *options = 0;
    *memory = descriptor;
    return kIOReturnSuccess;
}

// CopyClientMemoryForType — the BAR0..5 memory regions (T-dma-dart-dext).
// type is the BAR index (0..5); the dext returns the IOMemoryDescriptor
// for that BAR (the IOPCIDevice's memory mapping).
kern_return_t
IMPL(MacLinuxGPUUserClient, CopyClientMemoryForType)
{
    // On the delivery thread: a lookup or a provider call, never the
    // session's state (the owner's queue made the BO descriptors at BOMap).
    if (!ivars || ivars->stopping) return kIOReturnNotAttached;
    if (ivars->observer) return kIOReturnNotPermitted;
    if (ivars->linuxFile) return lx_copy_memory(this, type, options, memory);
    if (memory == nullptr || options == nullptr) {
        return kIOReturnBadArgument;
    }
    if (s_retainedPCI == nullptr) {
        return kIOReturnNotReady;
    }
    if (type == MLG_FW_MAILBOX_MEMORY_TYPE) {
        *options = 0;
        return copy_firmware_mailbox(memory);
    }
    if (ivars->sessionGeneration != __atomic_load_n(&s_sessionGeneration, __ATOMIC_ACQUIRE))
        return kIOReturnNotOpen;
    if (type >= 0x10000) {
        IOMemoryDescriptor *descriptor = client_memory_find(this, type);
        if (!descriptor) return kIOReturnBadArgument;
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
    MACLINUXGPU_LOG("client %llu: maps all of BAR%llu (%llu bytes) into its process (raw lease)",
                    ivars->clientID, (unsigned long long)type, (unsigned long long)barSize);
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

// DrmSelfTest: the kernel-queue CS self-test in a Linux process of its own,
// one at a time, admitted like the other observer reads (the upstream
// driver runs in an open session for the whole test).
static uint32_t s_selfTestRunning;
static_assert(sizeof(struct rt_cs_selftest_result) <= MLG_DRM_SELFTEST_RESULT_MAX,
              "DrmSelfTest result size");
static kern_return_t observer_drm_selftest(IOUserClientMethodArguments *arguments)
{
    uint64_t *out = arguments->scalarOutput;
    if (!out || arguments->scalarOutputCount < MLG_DRM_SELFTEST_WORDS ||
        arguments->structureInput || arguments->structureInputDescriptor ||
        arguments->structureOutputDescriptor ||
        arguments->structureOutputMaximumSize < sizeof(struct rt_cs_selftest_result))
        return kIOReturnBadArgument;
    if (__atomic_exchange_n(&s_selfTestRunning, 1u, __ATOMIC_ACQ_REL)) return kIOReturnBusy;
    if (!s_observerReads.enter()) {
        __atomic_store_n(&s_selfTestRunning, 0u, __ATOMIC_RELEASE);
        return kIOReturnNotReady;
    }
    struct rt_cs_selftest_result result;
    const int r = rt_cs_selftest_run(
        static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)), &result);
    const unsigned parked = rt_cs_selftest_parked();
    s_observerReads.leave();
    __atomic_store_n(&s_selfTestRunning, 0u, __ATOMIC_RELEASE);
    MACLINUXGPU_LOG("CS self-test: %d (failed step %u, passed %#x, compute %llu ns, sdma %llu ns, "
                    "ttm to GTT %llu bytes %llu ns, back %llu bytes %llu ns, parked %u)",
                    r, result.failed_step, result.passed,
                    (unsigned long long)result.compute_ns, (unsigned long long)result.sdma_ns,
                    (unsigned long long)result.gtt_moved, (unsigned long long)result.gtt_ns,
                    (unsigned long long)result.vram_moved, (unsigned long long)result.vram_ns, parked);
    arguments->structureOutput = OSData::withBytes(&result, sizeof(result));
    if (!arguments->structureOutput) return kIOReturnNoMemory;
    out[0] = (uint64_t)(int64_t)r;
    out[1] = parked;
    arguments->scalarOutputCount = MLG_DRM_SELFTEST_WORDS;
    return kIOReturnSuccess;
}

// Display: the in-driver display test (rt/display.h), one op at a time,
// admitted like the other observer reads (the upstream driver runs in an
// open session for the whole op). Every wait inside is upstream's own
// bounded wait.
static uint32_t s_displayRunning;
// Linux errno values the display ops report (rt/display.h and rt/surface.h
// return negative Linux errnos).
static constexpr int kLinuxENOENT = 2, kLinuxEIO = 5, kLinuxENOMEM = 12, kLinuxENOSPC = 28;

// A client memory import: the DMA mapping of the client's descriptor,
// released once the GPU can no longer reach it
// (rt_surface's provider release, after the BO is destroyed).
struct DisplayImport {
    uint64_t importID;
    uint64_t length;
};
static void display_import_release(void *context)
{
    auto *import = static_cast<DisplayImport *>(context);
    if (dext_dma_release_import(import->importID) != 0)
        MACLINUXGPU_LOG("display: import %llu release retained by the DMA seam",
                        (unsigned long long)import->importID);
    IOFree(import, sizeof(*import));
}

/* Stopped clients whose imports and output are still to release: on a
 * driver thread, under the display gate, so after any op of theirs still
 * finishing (display_stops_kick, from the stop and from each op's end). */
static void observer_display_client_stop_now(uint64_t clientID);
static bool display_try_acquire();
static void display_release();
static constexpr unsigned kDisplayStopsMax = 32;
static uint64_t s_displayStops[kDisplayStopsMax];
static unsigned s_displayStopCount;

static void display_stops_main(void *)
{
    for (;;) {
        IOLockLock(s_displayStopsLock);
        const uint64_t clientID = s_displayStopCount ? s_displayStops[--s_displayStopCount] : 0;
        IOLockUnlock(s_displayStopsLock);
        if (!clientID) break;
        observer_display_client_stop_now(clientID);
    }
    display_release(); // and kicks again, for a stop that came meanwhile
}

static void display_stops_kick()
{
    if (!s_displayStopsLock) return;
    IOLockLock(s_displayStopsLock);
    const bool pending = s_displayStopCount != 0;
    IOLockUnlock(s_displayStopsLock);
    if (!pending || !display_try_acquire()) return; // the op running kicks at its end
    const int started = rt_wait_pool_run(display_stops_main, nullptr);
    if (started) {
        MACLINUXGPU_LOG("display: stopped clients' imports and output not released: no driver thread (%d); "
                        "the session close releases them", started);
        __atomic_store_n(&s_displayRunning, 0u, __ATOMIC_RELEASE);
    }
}

static void observer_display_client_stop(uint64_t clientID)
{
    if (!s_displayStopsLock) return; // allocated at Start
    IOLockLock(s_displayStopsLock);
    const bool queued = s_displayStopCount < kDisplayStopsMax;
    if (queued) s_displayStops[s_displayStopCount++] = clientID;
    IOLockUnlock(s_displayStopsLock);
    if (!queued)
        MACLINUXGPU_LOG("display: client %llu's imports and output not released: %u stops pending; "
                        "the session close releases them", (unsigned long long)clientID, kDisplayStopsMax);
    display_stops_kick();
}

static void observer_display_client_stop_now(uint64_t clientID)
{
    if (!s_observerReads.enter()) return; // a session close released everything
    const unsigned released = rt_surface_remove_owner(clientID);
    bool output = false;
    if (__atomic_load_n(&s_displayOwner, __ATOMIC_ACQUIRE) == clientID) {
        rt_display_stop();
        __atomic_store_n(&s_displayOwner, 0, __ATOMIC_RELEASE);
        output = true;
    }
    s_observerReads.leave();
    if (released || output)
        MACLINUXGPU_LOG("display: client %llu stopped: %u surface(s) released%s",
                        (unsigned long long)clientID, released, output ? ", output off" : "");
}

// IMPORT, VERIFY, RELEASE, OUTPUT and PRESENT (session_state.h). Admitted
// and serialized like the other display ops. Statuses are Linux errnos in
// out[0]; nothing here falls back to another mechanism.
static kern_return_t display_frames(uint64_t clientID, IOUserClientMethodArguments *a,
                                    struct pci_dev *pdev)
{
    const uint64_t *in = a->scalarInput;
    uint64_t *out = a->scalarOutput;
    out[0] = 0;
    out[1] = 0;
    a->scalarOutputCount = MLG_DISPLAY_WORDS;
    switch (in[0]) {
    case MLG_DISPLAY_OP_IMPORT: {
        IOMemoryDescriptor *memory = a->structureInputDescriptor;
        uint64_t length = 0;
        if (!memory || a->structureInput || memory->GetLength(&length) != kIOReturnSuccess || !length)
            return kIOReturnBadArgument;
        const uint32_t width = (uint32_t)(in[1] >> 48), height = (uint32_t)(in[1] >> 32) & 0xffff;
        const uint32_t pitch = (uint32_t)in[1];
        uint64_t addresses[DEXT_DMA_IMPORT_SEGMENTS_MAX], lengths[DEXT_DMA_IMPORT_SEGMENTS_MAX];
        uint32_t count = DEXT_DMA_IMPORT_SEGMENTS_MAX;
        auto *import = static_cast<DisplayImport *>(IOMallocZero(sizeof(DisplayImport)));
        if (!import) { out[0] = (uint64_t)(int64_t)-kLinuxENOMEM; return kIOReturnSuccess; }
        import->length = length;
        if (dext_dma_import(memory, length, addresses, lengths, &count, &import->importID) != 0) {
            IOFree(import, sizeof(*import));
            MACLINUXGPU_LOG("display: IMPORT of %llu bytes: the platform DMA mapping was refused",
                            (unsigned long long)length);
            out[0] = (uint64_t)(int64_t)-kLinuxEIO;
            return kIOReturnSuccess;
        }
        // No CPU view: a mapping of the client's descriptor here is a
        // snapshot taken at this call, not its live pages (hardware run,
        // build 233); the GPU's DMA mapping is what follows the client.
        struct rt_surface_segment segments[DEXT_DMA_IMPORT_SEGMENTS_MAX];
        for (uint32_t i = 0; i < count; ++i) segments[i] = {addresses[i], lengths[i]};
        const struct rt_surface_provider provider = {display_import_release, import};
        struct rt_surface *surface = nullptr;
        int r = rt_surface_import(pdev, segments, count, length, width, height, pitch, &provider,
                                  &surface);
        if (r) {
            display_import_release(import); // the provider was not taken
        } else {
            const uint32_t handle = rt_surface_add(clientID, surface);
            if (!handle) {
                rt_surface_release(surface); // the provider follows the BO
                r = -kLinuxENOSPC;
            } else {
                out[1] = handle;
            }
        }
        MACLINUXGPU_LOG("display: IMPORT %ux%u pitch %u, %llu bytes in %u segment(s) -> %d (handle %llu)",
                        width, height, pitch, (unsigned long long)length, count, r,
                        (unsigned long long)out[1]);
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    case MLG_DISPLAY_OP_VERIFY: {
        if (a->structureInput || a->structureInputDescriptor ||
            a->structureOutputMaximumSize < sizeof(struct rt_surface_verify_result))
            return kIOReturnBadArgument;
        struct rt_surface *surface = rt_surface_get(clientID, (uint32_t)(in[1] >> 32));
        struct rt_surface_verify_result result{};
        int r = -kLinuxENOENT;
        if (surface) {
            r = rt_surface_verify(surface, (uint32_t)in[1], nullptr, 2000, &result);
            MACLINUXGPU_LOG("display: VERIFY seed %u -> %d (GPU %u, CPU %u mismatching dwords, %llu us)",
                            (uint32_t)in[1], r, result.gpu_mismatches, result.cpu_mismatches,
                            (unsigned long long)(result.gpu_ns / 1000));
        }
        a->structureOutput = OSData::withBytes(&result, sizeof(result));
        if (!a->structureOutput) return kIOReturnNoMemory;
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    case MLG_DISPLAY_OP_RELEASE:
        if (a->structureInput || a->structureInputDescriptor) return kIOReturnBadArgument;
        out[0] = (uint64_t)(int64_t)rt_surface_remove(clientID, (uint32_t)in[1]);
        return kIOReturnSuccess;
    case MLG_DISPLAY_OP_OUTPUT: {
        const OSData *data = a->structureInput;
        struct mlg_display_output request{};
        if (!data || data->getLength() != sizeof(request) ||
            a->structureOutputMaximumSize < sizeof(struct rt_display_report))
            return kIOReturnBadArgument;
        memcpy(&request, data->getBytesNoCopy(), sizeof(request));
        request.connector[sizeof(request.connector) - 1] = 0;
        struct rt_display_report report;
        const int r = rt_display_output(pdev, request.connector, request.width, request.height,
                                        (uint32_t)in[1], &report);
        if (!r) {
            __atomic_store_n(&s_displayOwner, clientID, __ATOMIC_RELEASE);
            displays_publish(pdev, report);	// the lit mode (MacLinuxGPUDisplays)
        }
        MACLINUXGPU_LOG("display: OUTPUT %s at %ux%u %llu mHz -> %d (commit %d)", request.connector,
                        request.width, request.height, (unsigned long long)in[1], r, report.commit_status);
        a->structureOutput = OSData::withBytes(&report, sizeof(report));
        if (!a->structureOutput) return kIOReturnNoMemory;
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    case MLG_DISPLAY_OP_PRESENT: {
        const OSData *data = a->structureInput;
        if (!data || data->getLength() < sizeof(struct mlg_display_present) ||
            a->structureOutputMaximumSize < sizeof(struct rt_display_present_stats))
            return kIOReturnBadArgument;
        const auto *request = static_cast<const struct mlg_display_present *>(data->getBytesNoCopy());
        if (request->count > MLG_DISPLAY_PRESENT_RECTS_MAX || request->moves > MLG_DISPLAY_PRESENT_MOVES_MAX ||
            data->getLength() > MLG_DISPLAY_PRESENT_BYTES_MAX ||
            data->getLength() != sizeof(*request) + request->count * sizeof(struct mlg_display_rect) +
                                     request->moves * sizeof(struct mlg_display_move))
            return kIOReturnBadArgument;
        const auto *moves = reinterpret_cast<const struct rt_display_move *>(request->rect + request->count);
        struct rt_display_present_stats stats{};
        int r = -kLinuxENOENT;
        if (__atomic_load_n(&s_displayOwner, __ATOMIC_ACQUIRE) != clientID) {
            /* Only the client that lit the output presents or reads it. */
        } else if (!request->count && !request->moves) {
            r = rt_display_stats(pdev, &stats);
        } else {
            /* The hold goes to the output's worker, which releases it once
             * the frame is copied (or replaced by a newer one). */
            struct rt_surface *surface = rt_surface_get_hold(clientID, (uint32_t)in[1]);
            if (surface)
                r = rt_display_present(pdev, surface,
                                       reinterpret_cast<const struct rt_surface_rect *>(request->rect),
                                       request->count, request->moves ? moves : nullptr, request->moves,
                                       request->capture_ns, &stats);
        }
        if (r && r != -kLinuxENOENT)
            MACLINUXGPU_LOG("display: PRESENT failed %d (worker error %d)", r, stats.error);
        a->structureOutput = OSData::withBytes(&stats, sizeof(stats));
        if (!a->structureOutput) return kIOReturnNoMemory;
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}

// ----------------------------------------------------------------
// Session calls off the delivery thread (session_state.h, "Calls that
// never sleep, and every other call").
// ----------------------------------------------------------------

// ExternalMethod's reference when a session call runs on the owner's queue
// (owner_job_main) rather than arriving from a client.
static const uint8_t kOwnerJob = 0;

struct OwnerResult {
    OwnerResult *next;
    uint64_t token;
    OSData *data;
};

struct ClientMemory {
    ClientMemory *next;
    uint64_t handle, type;
    IOMemoryDescriptor *memory;
};

static void owner_results_free(MacLinuxGPUUserClient *client)
{
    auto *iv = client->ivars;
    if (!iv->ownerLock) return;
    IOLockLock(iv->ownerLock);
    OwnerResult *list = iv->ownerResults;
    iv->ownerResults = nullptr;
    ClientMemory *memories = iv->memories;
    iv->memories = nullptr;
    IOLockUnlock(iv->ownerLock);
    while (memories) {
        ClientMemory *next = memories->next;
        memories->memory->release();
        IOFree(memories, sizeof(*memories));
        memories = next;
    }
    while (list) {
        OwnerResult *next = list->next;
        if (list->data) list->data->release();
        IOFree(list, sizeof(*list));
        list = next;
    }
    IOLockFree(iv->ownerLock);
    iv->ownerLock = nullptr;
}

// Keep a call's structure output for OWNER_RESULT (@data's reference
// passes to the list). Results nobody fetches go with the client.
static void owner_result_store(MacLinuxGPUUserClient *client, uint64_t token, OSData *data)
{
    auto *entry = static_cast<OwnerResult *>(IOMallocZero(sizeof(OwnerResult)));
    if (!entry) {
        data->release();
        return;
    }
    entry->token = token;
    entry->data = data;
    IOLockLock(client->ivars->ownerLock);
    entry->next = client->ivars->ownerResults;
    client->ivars->ownerResults = entry;
    IOLockUnlock(client->ivars->ownerLock);
}

// On the owner's queue, after BOMap: the descriptor a mapping of @type
// hands out (the BO's DMA buffer, or a KFD GTT BO's page ranges).
static void client_memory_stash(MacLinuxGPUUserClient *client, uint64_t handle, uint64_t type)
{
    void *cpu = nullptr;
    uint64_t size = 0;
    const int located = dext_compute_bo_memory((uint32_t)type, &cpu, &size);
    IOMemoryDescriptor *descriptor = nullptr;
    if (located == -EAGAIN_L && size)
        descriptor = copy_bo_ranges_descriptor((uint32_t)type, size);
    else if (located == 0 && cpu && size)
        descriptor = static_cast<IOMemoryDescriptor *>(dext_dma_copy_descriptor(cpu));
    if (!descriptor) return;  // the mapping then fails (kIOReturnBadArgument)
    auto *entry = static_cast<ClientMemory *>(IOMallocZero(sizeof(ClientMemory)));
    if (!entry) { descriptor->release(); return; }
    entry->handle = handle;
    entry->type = type;
    entry->memory = descriptor;
    IOLockLock(client->ivars->ownerLock);
    for (ClientMemory **link = &client->ivars->memories; *link; link = &(*link)->next) {
        if ((*link)->type != type) continue;
        ClientMemory *old = *link;  // mapped again: the newer descriptor
        *link = old->next;
        old->memory->release();
        IOFree(old, sizeof(*old));
        break;
    }
    entry->next = client->ivars->memories;
    client->ivars->memories = entry;
    IOLockUnlock(client->ivars->ownerLock);
}

// On the owner's queue, after BOFree.
static void client_memory_drop(MacLinuxGPUUserClient *client, uint64_t handle)
{
    ClientMemory *found = nullptr;
    IOLockLock(client->ivars->ownerLock);
    for (ClientMemory **link = &client->ivars->memories; *link; link = &(*link)->next) {
        if ((*link)->handle != handle) continue;
        found = *link;
        *link = found->next;
        break;
    }
    IOLockUnlock(client->ivars->ownerLock);
    if (found) {
        found->memory->release();
        IOFree(found, sizeof(*found));
    }
}

// On the delivery thread: the descriptor, retained, or nullptr.
static IOMemoryDescriptor *client_memory_find(MacLinuxGPUUserClient *client, uint64_t type)
{
    IOMemoryDescriptor *memory = nullptr;
    IOLockLock(client->ivars->ownerLock);
    for (ClientMemory *entry = client->ivars->memories; entry; entry = entry->next) {
        if (entry->type != type) continue;
        memory = entry->memory;
        memory->retain();
        break;
    }
    IOLockUnlock(client->ivars->ownerLock);
    return memory;
}

// OWNER_RESULT, on the delivery thread: a lookup under the client's lock.
static kern_return_t owner_result(MacLinuxGPUUserClient *client, IOUserClientMethodArguments *a)
{
    const uint64_t token = a->scalarInput[0];
    OSData *data = nullptr;
    bool fits = true;
    IOLockLock(client->ivars->ownerLock);
    for (OwnerResult **link = &client->ivars->ownerResults; *link; link = &(*link)->next) {
        if ((*link)->token != token) continue;
        if ((*link)->data->getLength() > a->structureOutputMaximumSize) {
            fits = false;  // kept: the caller asks again with room
            break;
        }
        OwnerResult *entry = *link;
        *link = entry->next;
        data = entry->data;
        IOFree(entry, sizeof(*entry));
        break;
    }
    IOLockUnlock(client->ivars->ownerLock);
    if (!fits) return kIOReturnNoSpace;
    if (!data) return kIOReturnNotFound;
    a->structureOutput = data;  // its reference passes to the reply
    a->scalarOutputCount = 0;
    return kIOReturnSuccess;
}

struct OwnerJob {
    MacLinuxGPUUserClient *client;
    OSAction *action;
    uint64_t selector;
    uint64_t token;
    uint64_t in[16];
    uint32_t nin, nout;
    uint64_t outputMax;
    OSData *input;                       // a copy of the structure input
    IOMemoryDescriptor *inputDescriptor; // a large structure input, retained
};

static void owner_job_free(OwnerJob *job)
{
    if (job->input) job->input->release();
    if (job->inputDescriptor) job->inputDescriptor->release();
    job->action->release();
    job->client->release();
    IOFree(job, sizeof(*job));
}

// On the owner's queue: the selector as it always ran there, then the
// completion (session_state.h's layout).
static void owner_job_main(OwnerJob *job)
{
    IOUserClientMethodArguments a{};
    uint64_t out[MLG_OWNER_ASYNC_WORDS] = {};
    a.version = kIOUserClientMethodArgumentsCurrentVersion;
    a.selector = job->selector;
    a.scalarInput = job->in;
    a.scalarInputCount = job->nin;
    a.structureInput = job->input;
    a.structureInputDescriptor = job->inputDescriptor;
    a.scalarOutput = out;
    a.scalarOutputCount = job->nout;
    a.structureOutputMaximumSize = job->outputMax;
    a.completion = job->action;
    const kern_return_t kr = job->client->ExternalMethod(job->selector, &a, nullptr, nullptr,
                                                         const_cast<uint8_t *>(&kOwnerJob));
    IOUserClientAsyncArgumentsArray data = {};
    if (job->selector == MLG_SELECTOR_EVENT_WAIT) {
        // Started: event_wait_main completes it. Not started: completed
        // here, in EVENT_WAIT's own layout, with the reason.
        const bool started = kr == kIOReturnSuccess && a.scalarOutputCount >= 1 &&
                             (int64_t)out[0] >= 0;
        if (!started) {
            data[0] = job->in[0];
            data[1] = kr == kIOReturnSuccess ? out[0] : (uint64_t)(int64_t)-22; /* EINVAL */
            data[2] = 2;  // the KFD wait failed
            job->client->AsyncCompletion(job->action, kIOReturnSuccess, data, MLG_EVENT_WAIT_WORDS);
        }
        if (a.structureOutput) a.structureOutput->release();
        owner_job_free(job);
        return;
    }
    const uint32_t n = kr == kIOReturnSuccess ?
        (a.scalarOutputCount < MLG_OWNER_ASYNC_SCALARS ? a.scalarOutputCount : MLG_OWNER_ASYNC_SCALARS) : 0;
    OSData *output = a.structureOutput;
    data[0] = job->token;
    data[1] = (uint64_t)(int64_t)kr;
    data[2] = n;
    data[3] = output ? output->getLength() : 0;
    for (uint32_t i = 0; i < n; ++i) data[MLG_OWNER_ASYNC_HEADER + i] = out[i];
    // In place before the completion, so a fetch that follows finds it.
    if (output) owner_result_store(job->client, job->token, output);
    job->client->AsyncCompletion(job->action, kIOReturnSuccess, data, MLG_OWNER_ASYNC_HEADER + n);
    owner_job_free(job);
}

// The delivery thread's part of a session call: copy the arguments, queue
// the call on the owner's queue, return.
static kern_return_t owner_call(MacLinuxGPUUserClient *client, uint64_t selector,
                                IOUserClientMethodArguments *a)
{
    if (a->scalarInputCount > 16 || (a->scalarInputCount && !a->scalarInput))
        return kIOReturnBadArgument;
    auto *job = static_cast<OwnerJob *>(IOMallocZero(sizeof(OwnerJob)));
    if (!job) return kIOReturnNoMemory;
    if (a->structureInput && a->structureInput->getLength()) {
        job->input = OSData::withBytes(a->structureInput->getBytesNoCopy(),
                                       a->structureInput->getLength());
        if (!job->input) { IOFree(job, sizeof(*job)); return kIOReturnNoMemory; }
    }
    job->client = client;
    job->action = a->completion;
    job->selector = selector;
    job->token = __atomic_add_fetch(&client->ivars->ownerToken, 1, __ATOMIC_ACQ_REL);
    job->nin = a->scalarInputCount;
    if (job->nin) memcpy(job->in, a->scalarInput, job->nin * sizeof(uint64_t));
    // As many scalar outputs as the caller asked for, up to what the
    // completion carries (EVENT_WAIT: its one status word).
    job->nout = a->scalarOutputCount < MLG_OWNER_ASYNC_SCALARS ? a->scalarOutputCount
                                                               : MLG_OWNER_ASYNC_SCALARS;
    if (selector == MLG_SELECTOR_EVENT_WAIT && job->nout < 1) job->nout = 1;
    // The caller's structure output room (a descriptor above 4096 bytes):
    // the output itself comes back through OWNER_RESULT.
    uint64_t room = a->structureOutputMaximumSize;
    if (a->structureOutputDescriptor) {
        uint64_t length = 0;
        if (a->structureOutputDescriptor->GetLength(&length) == kIOReturnSuccess) room = length;
    }
    job->outputMax = room;
    job->inputDescriptor = a->structureInputDescriptor;
    client->retain();
    job->action->retain();
    if (job->inputDescriptor) job->inputDescriptor->retain();
    client->ivars->ownerQueue->DispatchAsync(^{ owner_job_main(job); });
    if (a->scalarOutput && a->scalarOutputCount) {
        a->scalarOutput[0] = selector == MLG_SELECTOR_EVENT_WAIT ? 0 : job->token;
        a->scalarOutputCount = 1;
    }
    return kIOReturnSuccess;
}

// The calls that run on the delivery thread (mlg_call_runs_on_delivery):
// published state, a spinlock, or a client's own result list.
static kern_return_t direct_call(MacLinuxGPUUserClient *client, uint64_t selector,
                                 IOUserClientMethodArguments *a)
{
    const uint64_t *in = a->scalarInput;
    uint64_t *out = a->scalarOutput;
    switch (selector) {
    case MLG_SELECTOR_PING:
        if (a->scalarInputCount || !out || a->scalarOutputCount < 1) return kIOReturnBadArgument;
        out[0] = 0xA117AB1Eu;
        a->scalarOutputCount = 1;
        return kIOReturnSuccess;
    case MLG_SELECTOR_RUNTIME_BUILD: {
        // out[0]=magic, out[1]=ABI, out[2]=build when the session serves
        // compute, out[3] (when asked) the compiled build. Flags only.
        if (a->scalarInputCount || !out || a->scalarOutputCount < 3) return kIOReturnBadArgument;
        uint64_t build[4] = {0, 0, 0, 0};
        if (dext_compute_runtime_build_cached(build) != 0) return kIOReturnError;
        if (s_sessionClosing) build[2] = 0;
        const uint32_t words = a->scalarOutputCount >= 4 ? 4 : 3;
        for (uint32_t i = 0; i < words; ++i) out[i] = build[i];
        a->scalarOutputCount = words;
        return kIOReturnSuccess;
    }
    case MLG_SELECTOR_OWNER_RESULT:
        return owner_result(client, a);
    case MLG_SELECTOR_POWER:
        if (in[0] == MLG_POWER_OP_WAIT) {
            if (a->scalarInputCount != 2 || !a->completion) return kIOReturnBadArgument;
            return power_wait(client, a->completion, in[1]);
        }
        if (a->scalarInputCount != 1 || !out || a->scalarOutputCount < MLG_POWER_STATE_WORDS)
            return kIOReturnBadArgument;
        power_snapshot(out);
        a->scalarOutputCount = MLG_POWER_STATE_WORDS;
        return kIOReturnSuccess;
    case MLG_SELECTOR_QUERY_INFO:
        if (in[0] == DEXT_COMPUTE_QUERY_KERNEL_LOG) {
            if (!out || a->scalarOutputCount < 4) return kIOReturnBadArgument;
            uint64_t cursor = in[1], end = 0;
            uint8_t text[104] = {};
            const uint32_t words = a->scalarOutputCount < 16 ? a->scalarOutputCount : 16;
            const size_t copied = klog_read(&cursor, (char *)text,
                                             (words - 3) * sizeof(uint64_t), &end);
            memset(out, 0, words * sizeof(uint64_t));
            out[0] = end; out[1] = cursor; out[2] = copied;
            memcpy(out + 3, text, copied);
            a->scalarOutputCount = 3 + (uint32_t)((copied + 7) / 8);
            return kIOReturnSuccess;
        }
        if (in[0] == MLG_QUERY_POWER_STATE) {
            if (!out || a->scalarOutputCount < MLG_POWER_STATE_WORDS) return kIOReturnBadArgument;
            power_snapshot(out);
            a->scalarOutputCount = MLG_POWER_STATE_WORDS;
            return kIOReturnSuccess;
        }
        if (in[0] == DEXT_COMPUTE_QUERY_SESSION_STATE) {
            if (!out || a->scalarOutputCount < MLG_SESSION_STATE_WORDS) return kIOReturnBadArgument;
            session_state(out);
            a->scalarOutputCount = MLG_SESSION_STATE_WORDS;
            return kIOReturnSuccess;
        }
        // DEXT_COMPUTE_QUERY_PROBE_STATUS: probe progress, no MMIO.
        if (!out || a->scalarOutputCount < 5) return kIOReturnBadArgument;
        out[0] = s_probeAttempted;
        out[1] = s_modulesRunning;
        out[2] = (uint64_t)(int64_t)s_probeResult;
        out[3] = (uint64_t)dext_pci_transport_fault();
        out[4] = dext_pci_transport_fault_offset();
        a->scalarOutputCount = 5;
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

// SysfsRead and DrmInfo, bounded (session_state.h): the read on a pool
// thread, the call waiting at most MLG_BOUNDED_READ_MS for it.
struct BoundedRead {
    uint64_t selector;
    uint64_t in[2];
    OSData *input;
    uint64_t outputMax;
    uint64_t out[MLG_SYSFS_READ_WORDS];
    uint32_t outCount;
    kern_return_t result;
    OSData *output;
};
static uint32_t s_boundedReadRunning;

static void bounded_read_free(void *arg)
{
    auto *r = static_cast<BoundedRead *>(arg);
    if (r->input) r->input->release();
    if (r->output) r->output->release();
    IOFree(r, sizeof(*r));
    __atomic_store_n(&s_boundedReadRunning, 0u, __ATOMIC_RELEASE);
}

static void bounded_read_main(void *arg)
{
    auto *r = static_cast<BoundedRead *>(arg);
    IOUserClientMethodArguments a{};
    a.version = kIOUserClientMethodArgumentsCurrentVersion;
    a.selector = r->selector;
    a.scalarInput = r->in;
    a.scalarInputCount = 2;
    a.structureInput = r->input;
    a.scalarOutput = r->out;
    a.scalarOutputCount = MLG_SYSFS_READ_WORDS;
    a.structureOutputMaximumSize = r->outputMax;
    r->result = r->selector == MLG_SELECTOR_SYSFS_READ ? observer_sysfs_read(&a)
                                                        : observer_drm_info(&a);
    r->outCount = a.scalarOutputCount;
    r->output = a.structureOutput;
}

static kern_return_t bounded_read(uint64_t selector, IOUserClientMethodArguments *a)
{
    if (!a->scalarInput || a->scalarInputCount != 2 || !a->scalarOutput ||
        a->structureInputDescriptor || a->structureOutputDescriptor)
        return kIOReturnBadArgument;
    // One at a time: a read still running past its bound makes the next
    // one busy rather than piling up threads.
    if (__atomic_exchange_n(&s_boundedReadRunning, 1u, __ATOMIC_ACQ_REL)) return kIOReturnBusy;
    auto *r = static_cast<BoundedRead *>(IOMallocZero(sizeof(BoundedRead)));
    if (!r) {
        __atomic_store_n(&s_boundedReadRunning, 0u, __ATOMIC_RELEASE);
        return kIOReturnNoMemory;
    }
    r->selector = selector;
    r->in[0] = a->scalarInput[0];
    r->in[1] = a->scalarInput[1];
    r->outputMax = a->structureOutputMaximumSize;
    if (a->structureInput && a->structureInput->getLength()) {
        r->input = OSData::withBytes(a->structureInput->getBytesNoCopy(),
                                     a->structureInput->getLength());
        if (!r->input) { bounded_read_free(r); return kIOReturnNoMemory; }
    }
    const int ran = rt_bounded_run(bounded_read_main, r, bounded_read_free, MLG_BOUNDED_READ_MS);
    if (ran == -110 /* Linux ETIMEDOUT (rt/bounded.h) */) {
        MACLINUXGPU_LOG("observer read (selector %llu) still running after %u ms; answered with a timeout",
                        (unsigned long long)selector, MLG_BOUNDED_READ_MS);
        return kIOReturnTimeout;
    }
    if (ran) {
        bounded_read_free(r);
        return kIOReturnNoResources;
    }
    const kern_return_t kr = r->result;
    const uint32_t words = r->outCount < a->scalarOutputCount ? r->outCount : a->scalarOutputCount;
    for (uint32_t i = 0; i < words; ++i) a->scalarOutput[i] = r->out[i];
    a->scalarOutputCount = words;
    a->structureOutput = r->output;  // its reference passes to the reply
    r->output = nullptr;
    bounded_read_free(r);
    return kr;
}

// A synchronous call of a selector that must be called async: refused,
// and logged once per client (an old client library or runtime).
static kern_return_t refuse_sync(MacLinuxGPUUserClient *client, uint64_t selector)
{
    if (!client->ivars->syncRefusalLogged) {
        client->ivars->syncRefusalLogged = true;
        MACLINUXGPU_EVENT("client %llu: selector %llu called synchronously; it can sleep and must be "
                          "called async (a client library or runtime older than this driver: "
                          "rebuild it)", client->ivars->clientID, (unsigned long long)selector);
    }
    return kIOReturnNotPermitted;
}

// A client's last display op result (MacLinuxGPUUserClient_IVars::displayResults).
struct DisplayResultSlot {
    uint32_t refs;
    IOLock *lock;
    uint64_t token;
    kern_return_t result;
    uint64_t out[2];
    OSData *data;
};
static DisplayResultSlot *display_slot_new()
{
    auto *slot = static_cast<DisplayResultSlot *>(IOMallocZero(sizeof(DisplayResultSlot)));
    if (!slot) return nullptr;
    slot->lock = IOLockAlloc();
    if (!slot->lock) { IOFree(slot, sizeof(*slot)); return nullptr; }
    slot->refs = 1;
    return slot;
}
static void display_slot_put(DisplayResultSlot *slot)
{
    if (!slot || __atomic_sub_fetch(&slot->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (slot->data) slot->data->release();
    IOLockFree(slot->lock);
    IOFree(slot, sizeof(*slot));
}

// The display gate: one op that can sleep at a time. A call that finds it
// taken is answered kIOReturnBusy at once; nothing waits for it. Releasing
// it runs any stopped clients' cleanup (display_stops_kick).
static void display_stops_kick();
static bool display_try_acquire()
{
    return !__atomic_exchange_n(&s_displayRunning, 1u, __ATOMIC_ACQ_REL);
}
static void display_release()
{
    __atomic_store_n(&s_displayRunning, 0u, __ATOMIC_RELEASE);
    display_stops_kick();
}

// PRESENT, synchronous: it never sleeps (rt_display_present returns -EBUSY
// rather than wait for an op that holds the display), so it takes no
// display gate, only the observer admission.
static kern_return_t observer_display_present(uint64_t clientID, IOUserClientMethodArguments *arguments)
{
    if (arguments->scalarOutputCount < MLG_DISPLAY_WORDS) return kIOReturnBadArgument;
    if (!s_observerReads.enter()) return kIOReturnNotReady;
    const kern_return_t ret = display_frames(clientID, arguments,
        static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)));
    s_observerReads.leave();
    return ret;
}

// PROBE, SHOW, OFF, STATUS and MODES: their arguments, and the connector
// name of SHOW and MODES in @name.
static bool observer_display_valid(IOUserClientMethodArguments *arguments, char name[RT_DISPLAY_NAME_BYTES])
{
    const uint64_t *in = arguments->scalarInput;
    const OSData *nameData = arguments->structureInput;
    memset(name, 0, RT_DISPLAY_NAME_BYTES);
    if (arguments->structureInputDescriptor ||
        arguments->structureOutputMaximumSize < sizeof(struct rt_display_report))
        return false;
    if (nameData && nameData->getLength()) {
        const char *bytes = static_cast<const char *>(nameData->getBytesNoCopy());
        size_t length = nameData->getLength();
        if ((in[0] != MLG_DISPLAY_OP_SHOW && in[0] != MLG_DISPLAY_OP_MODES) || !bytes)
            return false;
        if (bytes[length - 1] == '\0') --length;
        if (!length || length > MLG_DISPLAY_NAME_MAX) return false;
        for (size_t i = 0; i < length; ++i) {
            if (bytes[i] <= ' ' || bytes[i] > '~') return false;
            name[i] = bytes[i];
        }
    }
    return in[0] != MLG_DISPLAY_OP_MODES || name[0];
}

// PROBE, SHOW, OFF, STATUS and MODES, on a driver thread, admitted.
static kern_return_t observer_display_run(IOUserClientMethodArguments *arguments, const char *name,
                                          struct pci_dev *pdev)
{
    const uint64_t *in = arguments->scalarInput;
    uint64_t *out = arguments->scalarOutput;
    if (in[0] == MLG_DISPLAY_OP_MODES) {
        struct rt_display_modes modes;
        const int r = rt_display_modes(pdev, name, &modes);
        arguments->structureOutput = OSData::withBytes(&modes, sizeof(modes));
        if (!arguments->structureOutput) return kIOReturnNoMemory;
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    struct rt_display_report report;
    int r;
    if (in[0] == MLG_DISPLAY_OP_STATUS) {
        // Polled by a display agent: cached state only, not logged.
        r = rt_display_status(pdev, &report);
        if (r == 0) displays_publish(pdev, report);
        arguments->structureOutput = OSData::withBytes(&report, sizeof(report));
        if (!arguments->structureOutput) return kIOReturnNoMemory;
        out[0] = (uint64_t)(int64_t)r;
        return kIOReturnSuccess;
    }
    if (in[0] == MLG_DISPLAY_OP_PROBE) {
        r = rt_display_probe(pdev, &report);
    } else if (in[0] == MLG_DISPLAY_OP_SHOW) {
        MACLINUXGPU_LOG("display test: show pattern %llu on %s", (unsigned long long)in[1],
                        name[0] ? name : "every connected output");
        r = rt_display_show(pdev, name[0] ? name : nullptr, (uint32_t)in[1], &report);
    } else {
        r = rt_display_off(pdev, &report);
        __atomic_store_n(&s_displayOwner, 0, __ATOMIC_RELEASE);
    }
    if (r == 0) displays_publish(pdev, report);
    unsigned connected = 0, lit = 0;
    for (uint32_t i = 0; i < report.connectors && i < RT_DISPLAY_CONNECTORS_MAX; ++i) {
        connected += report.connector[i].status == 1;
        lit += report.connector[i].lit != 0;
    }
    MACLINUXGPU_LOG("display test: op %llu -> %d (probe %d, commit %d, restore %d; %u connector(s), "
                    "%u connected, %u lit, fb %ux%u, fill %llu ms, commit %llu ms)",
                    (unsigned long long)in[0], r, report.probe_status, report.commit_status,
                    report.restore_status, report.connectors, connected, lit, report.fb_width,
                    report.fb_height, (unsigned long long)(report.fill_ns / 1000000),
                    (unsigned long long)(report.commit_ns / 1000000));
    arguments->structureOutput = OSData::withBytes(&report, sizeof(report));
    if (!arguments->structureOutput) return kIOReturnNoMemory;
    out[0] = (uint64_t)(int64_t)r;
    return kIOReturnSuccess;
}

// An op that can sleep, on a wait-pool thread: run, keep the result for
// RESULT, then complete the client's call.
struct DisplayJob {
    MacLinuxGPUUserClient *client;
    OSAction *action;
    uint64_t clientID;
    uint64_t token;
    uint64_t in[3];
    char name[RT_DISPLAY_NAME_BYTES];
    OSData *input;                  // a copy of the structure input
    IOMemoryDescriptor *descriptor; // IMPORT's memory, retained
    uint64_t outputMax;
    DisplayResultSlot *results;     // the client's, referenced
};

static void display_job_free(DisplayJob *job)
{
    if (job->input) job->input->release();
    if (job->descriptor) job->descriptor->release();
    display_slot_put(job->results);
    job->action->release();
    job->client->release();
    IOFree(job, sizeof(*job));
}

static void display_job_main(void *arg)
{
    auto *job = static_cast<DisplayJob *>(arg);
    IOUserClientMethodArguments a{};
    uint64_t out[MLG_DISPLAY_WORDS] = {};
    a.scalarInput = job->in;
    a.scalarInputCount = 3;
    a.structureInput = job->input;
    a.structureInputDescriptor = job->descriptor;
    a.scalarOutput = out;
    a.scalarOutputCount = MLG_DISPLAY_WORDS;
    a.structureOutputMaximumSize = job->outputMax;
    auto *pdev = static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice));
    const kern_return_t kr = job->in[0] >= MLG_DISPLAY_OP_IMPORT ? display_frames(job->clientID, &a, pdev)
                                                                 : observer_display_run(&a, job->name, pdev);
    s_observerReads.leave();
    display_release();
    OSData *output = a.structureOutput;
    DisplayResultSlot *slot = job->results;
    IOLockLock(slot->lock);
    if (slot->data) slot->data->release();
    slot->token = job->token;
    slot->result = kr;
    slot->out[0] = out[0];
    slot->out[1] = out[1];
    slot->data = output; // the reference structureOutput held
    if (output) output->retain(); // and the completion's length below
    IOLockUnlock(slot->lock);
    IOUserClientAsyncArgumentsArray data = {};
    data[0] = job->token;
    data[1] = (uint64_t)(int64_t)kr;
    data[2] = out[0];
    data[3] = out[1];
    data[4] = output ? output->getLength() : 0;
    if (output) output->release();
    job->client->AsyncCompletion(job->action, kIOReturnSuccess, data, MLG_DISPLAY_ASYNC_WORDS);
    display_job_free(job);
}

// RESULT: the structure output of the client's last op, once.
static kern_return_t display_result(MacLinuxGPUUserClient *client, IOUserClientMethodArguments *a)
{
    DisplayResultSlot *slot = client->ivars->displayResults;
    if (a->scalarOutputCount < MLG_DISPLAY_WORDS) return kIOReturnBadArgument;
    IOLockLock(slot->lock);
    if (slot->token != a->scalarInput[1]) {
        IOLockUnlock(slot->lock);
        return kIOReturnNotFound;
    }
    const kern_return_t kr = slot->result;
    a->scalarOutput[0] = slot->out[0];
    a->scalarOutput[1] = slot->out[1];
    a->scalarOutputCount = MLG_DISPLAY_WORDS;
    OSData *data = slot->data;
    slot->data = nullptr;
    slot->token = 0;
    IOLockUnlock(slot->lock);
    if (data && data->getLength() > a->structureOutputMaximumSize) {
        data->release();
        return kIOReturnNoSpace;
    }
    a->structureOutput = data; // its reference passes to the reply
    return kr;
}

// The display selector, from an observer's call: PRESENT and RESULT answer
// at once; everything else is queued for a driver thread (display_job_main)
// and completes through the call's async completion. Nothing that can
// sleep runs on the call: a GPU that stops answering would otherwise hold
// every client's calls with it (build 241: an OUTPUT waiting for a hung
// ring froze the driver).
static kern_return_t display_call(MacLinuxGPUUserClient *client, uint64_t clientID,
                                  IOUserClientMethodArguments *a)
{
    const uint64_t *in = a->scalarInput;
    if (!in || a->scalarInputCount != 3 || !a->scalarOutput || a->scalarOutputCount < 1 ||
        a->structureOutputDescriptor || !mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, in, 3))
        return kIOReturnBadArgument;
    if (in[0] == MLG_DISPLAY_OP_PRESENT) return observer_display_present(clientID, a);
    if (in[0] == MLG_DISPLAY_OP_RESULT) return display_result(client, a);
    if (!a->completion || a->scalarOutputCount < MLG_DISPLAY_WORDS) return kIOReturnBadArgument;
    char name[RT_DISPLAY_NAME_BYTES] = {};
    if (in[0] < MLG_DISPLAY_OP_IMPORT && !observer_display_valid(a, name)) return kIOReturnBadArgument;
    auto *job = static_cast<DisplayJob *>(IOMallocZero(sizeof(DisplayJob)));
    if (!job) return kIOReturnNoMemory;
    if (a->structureInput && a->structureInput->getLength()) {
        job->input = OSData::withBytes(a->structureInput->getBytesNoCopy(), a->structureInput->getLength());
        if (!job->input) { IOFree(job, sizeof(*job)); return kIOReturnNoMemory; }
    }
    if (!display_try_acquire()) {
        if (job->input) job->input->release();
        IOFree(job, sizeof(*job));
        return kIOReturnBusy;
    }
    if (!s_observerReads.enter()) {
        display_release();
        if (job->input) job->input->release();
        IOFree(job, sizeof(*job));
        return kIOReturnNotReady;
    }
    job->client = client;
    job->action = a->completion;
    job->clientID = clientID;
    job->token = ++client->ivars->displayToken;
    memcpy(job->in, in, sizeof(job->in));
    memcpy(job->name, name, sizeof(job->name));
    job->descriptor = a->structureInputDescriptor;
    job->outputMax = a->structureOutputMaximumSize;
    job->results = client->ivars->displayResults;
    __atomic_add_fetch(&job->results->refs, 1, __ATOMIC_ACQ_REL);
    client->retain();
    job->action->retain();
    if (job->descriptor) job->descriptor->retain();
    const int started = rt_wait_pool_run(display_job_main, job);
    if (started) {
        s_observerReads.leave();
        display_release();
        display_job_free(job);
        MACLINUXGPU_LOG("display: op %llu not started: no driver thread (%d)", (unsigned long long)in[0], started);
        return kIOReturnNoResources;
    }
    a->scalarOutput[0] = 0;
    a->scalarOutput[1] = job->token;
    a->scalarOutputCount = MLG_DISPLAY_WORDS;
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
    (void)dispatch;
    if (!arguments) return kIOReturnBadArgument;
    if (reference != &kOwnerJob) {
        // From a client, on the delivery thread every client's calls share:
        // nothing here may sleep or wait for the owner's queue.
        if (!ivars || ivars->stopping) return kIOReturnNotAttached;
        const uint64_t *in = arguments->scalarInput;
        const uint32_t nin = arguments->scalarInputCount;
        if (ivars->observer) {
            if (!mlg_observer_selector_allowed(selector, in, nin))
                return kIOReturnNotPermitted;
            if (selector == MLG_SELECTOR_SYSFS_READ || selector == MLG_SELECTOR_DRM_INFO)
                return bounded_read(selector, arguments);
            if (selector == MLG_SELECTOR_DISPLAY) return display_call(this, ivars->clientID, arguments);
        } else if (ivars->linuxFile) {
            // Its process's system calls, the GPU's initialization (with the
            // host window it needs) and the cached queries.
            if (selector >= MLG_SELECTOR_LX_FIRST && selector <= MLG_SELECTOR_LX_LAST)
                return lx_external_method(this, selector, arguments);
            if (selector != kMacAMDGPUMethodPing && selector != kMacAMDGPUMethodRuntimeBuild &&
                selector != kMacAMDGPUMethodQueryInfo && selector != kMacAMDGPUMethodInitDevice &&
                selector != kMacAMDGPUMethodHostWindow && selector != MLG_SELECTOR_OWNER_RESULT)
                return kIOReturnUnsupported;
        }
        if (mlg_call_runs_on_delivery(selector, in, nin))
            return direct_call(this, selector, arguments);
        if (selector == MLG_SELECTOR_OWNER_RESULT) return kIOReturnBadArgument;
        if (!arguments->completion) return refuse_sync(this, selector);
        return owner_call(this, selector, arguments);
    }
    // On the owner's queue (owner_job_main): the session call itself.
    if (!ivars || ivars->stopping || ivars->ownerDriver != s_driver ||
        (s_stopping && !ivars->observer))
        return kIOReturnNotAttached;
    ComputeClientScope clientScope(ivars->clientID);
    if (!ivars->observer && !ivars->linuxFile) record_client_identity(this);
    if (ivars->observer) {
        // Cached state and the entitled release only; never PCI or the GPU.
        if (!mlg_observer_selector_allowed(selector, arguments->scalarInput,
                                           arguments->scalarInputCount))
            return kIOReturnNotPermitted;
    } else if (s_sessionClosing && selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodQueryInfo &&
        selector != kMacAMDGPUMethodRuntimeBuild && selector != kMacAMDGPUMethodPing &&
        selector != kMacAMDGPUMethodReleaseQuarantine && selector != MLG_SELECTOR_POWER &&
        selector != MLG_SELECTOR_RETIRE)
        return kIOReturnBusy;
    if (!ivars->observer && selector >= kMacAMDGPUMethodBOAlloc &&
        selector != kMacAMDGPUMethodQueryInfo &&
        selector != kMacAMDGPUMethodRuntimeBuild &&
        selector != kMacAMDGPUMethodHostWindow &&
        selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodReleaseQuarantine &&
        selector != MLG_SELECTOR_POWER && selector != MLG_SELECTOR_RETIRE &&
        ivars->sessionGeneration != s_sessionGeneration)
        return kIOReturnNotOpen;
    // Suspending, suspended or resuming: no new work for the GPU. Nothing
    // was submitted; the client retries after resume (power_state.h).
    if (!ivars->observer && !mlg_power_admits(s_power.state, selector))
        return kIOReturnOffline;

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
            MACLINUXGPU_EVENT("initialization: runtime device allocation failed");
            close_session(ivars->ownerDriver);
            return kIOReturnNotReady;
        }
        ret = prepare_interrupts(ivars->ownerDriver);
        if (ret != kIOReturnSuccess) {
            MACLINUXGPU_EVENT("initialization: interrupt preparation failed (%#x)", ret);
            close_session(ivars->ownerDriver);
            return ret;
        }

        // Upstream probe can free DMA during its own error unwind, before
        // returning here. Keep that backing pinned until success or FLR.
        const int probeHeld = dext_dma_begin_probe();
        if (probeHeld != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_PROBE_HOLD, probeHeld);
            MACLINUXGPU_EVENT("initialization: cannot reserve probe DMA backing (%d)", probeHeld);
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
            MACLINUXGPU_EVENT("upstream PCI probe failed: %d", s_probeResult);
            if (rt_pci_probe_cleanup_retained(rt_device_get_pdev(s_rtDevice))) {
                s_dmaQuarantined = true;
                note_quarantine(MLG_QUARANTINE_PROBE_RETAINED, s_probeResult);
                MACLINUXGPU_EVENT("upstream failed-probe ownership retained; preserving modules, device and DMA backing");
            }
            close_session(ivars->ownerDriver);
            return kIOReturnError;
        }
        MACLINUXGPU_EVENT("upstream AMDGPU PCI probe completed");
        identity_after_probe(ivars->ownerDriver,
                             static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)));
        int computeResult = dext_compute_start(
            static_cast<struct pci_dev *>(rt_device_get_pdev(s_rtDevice)));
        if (computeResult != 0) {
            const char *failedStep = NULL;
            int failedError = 0;
            dext_compute_start_failure(&failedStep, &failedError);
            MACLINUXGPU_EVENT("compute initialization failed: %d (%s: %d)", computeResult,
                            failedStep ? failedStep : "unknown", failedError);
            close_session(ivars->ownerDriver);
            return kIOReturnNotReady;
        }
        const int probeCommitted = dext_dma_commit_probe();
        if (probeCommitted != 0) {
            s_dmaQuarantined = true;
            note_quarantine(MLG_QUARANTINE_PROBE_COMMIT, probeCommitted);
            MACLINUXGPU_EVENT("probe DMA cleanup failed (%d); session quarantined", probeCommitted);
            close_session(ivars->ownerDriver);
            return kIOReturnError;
        }
        s_observerReads.open();
        s_lxCalls.open();
        power_session_started();
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
        // Device power state: cached, as the session state.
        if (in && arguments->scalarInputCount == 1 && in[0] == MLG_QUERY_POWER_STATE) {
            if (!out || arguments->scalarOutputCount < MLG_POWER_STATE_WORDS)
                return kIOReturnBadArgument;
            power_snapshot(out);
            arguments->scalarOutputCount = MLG_POWER_STATE_WORDS;
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
        if (r == 0) client_memory_drop(this, arguments->scalarInput[0]);
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
        client_memory_stash(this, arguments->scalarInput[0], out[0]);
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

    case MLG_SELECTOR_POWER: {
        // in[0]=op (power_state.h); WAIT: in[1]=known generation, async.
        // Others: out = the power snapshot.
        if (!in || arguments->scalarInputCount < 1) return kIOReturnBadArgument;
        if (in[0] == MLG_POWER_OP_WAIT) {
            if (arguments->scalarInputCount != 2 || !arguments->completion) return kIOReturnBadArgument;
            return power_wait(this, arguments->completion, in[1]);
        }
        if (arguments->scalarInputCount != 1 || !out ||
            arguments->scalarOutputCount < MLG_POWER_STATE_WORDS)
            return kIOReturnBadArgument;
        if (in[0] == MLG_POWER_OP_PREPARE || in[0] == MLG_POWER_OP_RESUME) {
            if (ivars->observer) {
                OSDictionary *entitlements = nullptr;
                bool entitled = false;
                if (CopyClientEntitlements(&entitlements) == kIOReturnSuccess && entitlements) {
                    entitled = entitlements->getObject(MLG_SESSION_RELEASE_ENTITLEMENT) == kOSBooleanTrue;
                    entitlements->release();
                }
                if (!entitled) return kIOReturnNotPrivileged;
            }
            if (in[0] == MLG_POWER_OP_PREPARE)
                power_client_prepare(ivars->ownerDriver, ivars->clientID);
            else
                power_client_release(ivars->ownerDriver, ivars->clientID, MLG_POWER_CAUSE_CLIENT_RESUME);
        } else if (in[0] != MLG_POWER_OP_QUERY) {
            return kIOReturnBadArgument;
        }
        power_snapshot(out);
        arguments->scalarOutputCount = MLG_POWER_STATE_WORDS;
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

    case MLG_SELECTOR_EVENT: {
        if (!in || arguments->scalarInputCount != 2 || !out ||
            arguments->scalarOutputCount < MLG_EVENT_WORDS || in[0] > MLG_EVENT_OP_SET ||
            in[1] > UINT32_MAX || arguments->structureInput)
            return kIOReturnBadArgument;
        uint64_t values[3] = {};
        const int r = dext_compute_event((uint32_t)in[0], (uint32_t)in[1], values);
        out[0] = (uint64_t)(int64_t)event_errno(r);
        out[1] = values[0];
        out[2] = values[1];
        out[3] = values[2];
        arguments->scalarOutputCount = MLG_EVENT_WORDS;
        return kIOReturnSuccess;
    }

    case MLG_SELECTOR_EVENT_WAIT: {
        const OSData *ids = arguments->structureInput;
        if (!in || arguments->scalarInputCount != 4 || !out || arguments->scalarOutputCount < 1 ||
            !arguments->completion || !ids || in[1] == 0 || in[1] > MLG_EVENT_WAIT_IDS ||
            in[2] > 1 || in[3] > UINT32_MAX ||
            ids->getLength() != in[1] * sizeof(uint32_t))
            return kIOReturnBadArgument;
        struct rt_kfd_wait *wait = nullptr;
        int r = dext_compute_event_wait_begin(static_cast<const uint32_t *>(ids->getBytesNoCopy()),
                                              (uint32_t)in[1], (int)in[2], (uint32_t)in[3], &wait);
        if (!r) {
            auto *job = static_cast<EventWaitJob *>(IOMallocZero(sizeof(EventWaitJob)));
            if (!job) {
                r = -ENOMEM_L;
            } else {
                job->client = this;
                job->action = arguments->completion;
                job->wait = wait;
                job->token = in[0];
                retain();
                job->action->retain();
                const int started = rt_wait_pool_run(event_wait_main, job);
                if (started) {
                    // Not started: the registered wait ends without sleeping.
                    struct rt_kfd_wait *unused = job->wait;
                    job->action->release();
                    release();
                    IOFree(job, sizeof(*job));
                    rt_kfd_wait_cancel(unused);
                    out[0] = (uint64_t)(int64_t)started;	/* Linux -EAGAIN / -ENOMEM */
                    arguments->scalarOutputCount = 1;
                    return kIOReturnSuccess;
                }
            }
            if (r) rt_kfd_wait_cancel(wait);
        }
        out[0] = (uint64_t)(int64_t)event_errno(r);
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case MLG_SELECTOR_DRM_SELFTEST:
        // The observer's self-test: GPU work for seconds, so an async call
        // on this (the owner's) queue.
        if (!ivars->observer) return kIOReturnUnsupported;
        return observer_drm_selftest(arguments);

    case MLG_SELECTOR_RETIRE: {
        // Hand the GPU to a replacement driver (session_state.h). Entitled:
        // it ends every client's session.
        if (!mlg_retire_args_valid(in, arguments->scalarInputCount) ||
            arguments->structureInput || arguments->structureInputDescriptor || !out ||
            arguments->scalarOutputCount < MLG_RETIRE_WORDS) return kIOReturnBadArgument;
        OSDictionary *entitlements = nullptr;
        bool entitled = false;
        if (CopyClientEntitlements(&entitlements) == kIOReturnSuccess && entitlements) {
            entitled = entitlements->getObject(MLG_SESSION_RELEASE_ENTITLEMENT) == kOSBooleanTrue;
            entitlements->release();
        }
        if (!entitled) return kIOReturnNotPrivileged;
        const bool participant = !ivars->observer && ivars->sessionGeneration == s_sessionGeneration;
        const uint32_t others = s_participants - (participant && s_participants ? 1u : 0u);
        uint64_t result[MLG_RETIRE_WORDS] = {};
        retire_driver(ivars->ownerDriver, in[0], (in[1] & MLG_RETIRE_FORCE) != 0, others, result);
        for (uint32_t i = 0; i < MLG_RETIRE_WORDS; ++i) out[i] = result[i];
        arguments->scalarOutputCount = MLG_RETIRE_WORDS;
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
