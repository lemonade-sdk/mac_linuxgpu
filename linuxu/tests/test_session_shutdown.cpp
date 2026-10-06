/* Execute extracted production shutdown functions with no DriverKit calls. */
#include "observer_gate.h"
#include "raw_bar_lease.h"
#include "session_state.h"
#include <Block.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <algorithm>
#include <vector>
#include <unistd.h>
#include "retained_log.h"

using kern_return_t = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnError,
    kIOReturnNotAttached, kIOReturnNotReady, kIOReturnNotOpen,
    kIOReturnNoResources, kIOReturnNotPermitted,
};
enum { DEXT_COMPUTE_STAGE_NONE, kMacAMDGPUMethodShutdownGPU = 42 };
enum { DEXT_PCI_FAULT_NONE = 0, DEXT_PCI_FAULT_CONFIG = 1, DEXT_PCI_FAULT_MMIO = 2, DEXT_PCI_FAULT_QUARANTINE = 4 };
#define SUPERDISPATCH 0
#define IMPL(cls, method) cls::method(IOService *provider)
static std::string platformLog;
static void platformLogSink(const char *text) {
    // A platform sink may query the cached log: do not retain its lock here.
    uint64_t cursor = UINT64_MAX, end = 0;
    assert(klog_read(&cursor, nullptr, 0, &end) == 0 && cursor == end);
    platformLog += text;
}
#define MACLINUXGPU_LOG(...) maclinuxgpu::RetainedLog(platformLogSink, __VA_ARGS__)
#define MACLINUXGPU_EVENT(...) maclinuxgpu::RetainedEvent(platformLogSink, __VA_ARGS__)

static std::string retainedLog() {
    char bytes[LINUXU_KLOG_CAPACITY];
    uint64_t cursor = 0;
    return std::string(bytes, klog_read(&cursor, bytes, sizeof(bytes), nullptr));
}
static void expectLog(const char *message) {
    const auto text = retainedLog();
    assert(text == platformLog);
    assert(text.find(message) != std::string::npos);
}
static void checkLogFormat() {
    unsigned evaluations = 0;
    MACLINUXGPU_LOG("failure=%d flags=%#x percent=%% text=%s once=%u", -38, 0xab,
        "%{private}s", ++evaluations);
    assert(evaluations == 1);
    expectLog("mac.linuxgpu: failure=-38 flags=0xab percent=% text=%{private}s once=1\n");
    const std::string large(8192, 'X');
    const size_t before = platformLog.size();
    MACLINUXGPU_LOG("%s", large.c_str());
    assert(platformLog.size() - before == LINUXU_KLOG_MESSAGE_CAPACITY - 1);
    expectLog("... [truncated]\n");
    // Events carry the prefix the unified-log predicate matches.
    MACLINUXGPU_EVENT("device removed (%s)", "test");
    expectLog("mac.linuxgpu: EVENT device removed (test)\n");
}

static std::vector<std::string> events;
static bool irqDrained, endpointReset, dmaCompleted;
static unsigned driverStops, clientStops;
static int holdError, computeError, irqError, resetError, dmaFiniError, isolationError;
static unsigned resetFailures = UINT32_MAX; // resets that fail with resetError
static int transportFault = DEXT_PCI_FAULT_NONE;
static bool probeCleanupRetained, computeQuiescent = true;
// The visible-VRAM aperture upstream leaves ioremapped after drm_dev_unplug().
static int bar0Aliases;
static unsigned pciOpens;
static void (*irqCompletion)(void *);
static void *irqContext;

struct IOService {
    unsigned references = 1;
    void retain() { ++references; }
    void release() { assert(references > 1); --references; }
};
struct IOPCIDevice : IOService {};
struct IODispatchQueue {
    using Block = void (^)(void);
    std::deque<Block> pending;
    // The owner's queue as a client's Stop sees it: nothing else is queued
    // there in these scenarios, so its block runs at once.
    bool runsAtOnce = false;
    void DispatchAsync(Block block) {
        if (runsAtOnce) { block(); return; }
        assert(irqDrained);
        events.push_back("enqueue_finish");
        pending.push_back(Block_copy(block));
    }
    void drain() {
        while (!pending.empty()) {
            auto block = pending.front(); pending.pop_front();
            block(); Block_release(block);
        }
    }
};
struct IOUserClientMethodArguments {
    uint32_t scalarInputCount = 0;
    uint64_t *scalarOutput = nullptr;
    uint32_t scalarOutputCount = 2;
};
struct MacLinuxGPUUserClient_IVars;
struct IOLock;
static IODispatchQueue s_ownerQueueAtOnce{{}, true};
struct MacLinuxGPU : IOService {
    void FinishSession();
    void FinishStop(IOService *provider);
    kern_return_t Stop(IOService *provider);
    kern_return_t Stop(IOService *, int) { assert(false); return kIOReturnError; }
    kern_return_t Terminate(uint64_t options);
};
struct MacLinuxGPUUserClient : IOService {
    MacLinuxGPUUserClient_IVars *ivars = nullptr;
    unsigned superStops = 0;
    void FinishStop(IOService *provider);
    kern_return_t Stop(IOService *provider);
    kern_return_t Stop(IOService *, int) { ++superStops; return kIOReturnSuccess; }
    kern_return_t shutdown(IOUserClientMethodArguments *arguments);
    kern_return_t failedProbe();
};

static int dext_dma_begin_shutdown();
static int dext_compute_stop();
static void linuxu_driver_shutdown();
static void dext_dma_quarantine();
static int dext_pci_quarantine();
static int dext_irq_fini_async(void (*)(void *), void *);
static void rt_device_free(void *);
static int dext_pci_shutdown_reset();
static void dext_compute_set_pci_open(bool);
static void dext_compute_set_stage(int);
static int dext_dma_fini();
static void dext_close();
static void rt_gart_reset();
static void *rt_device_get_pdev(void *);
static int rt_pci_probe_cleanup_retained(void *);
static int dext_pci_transport_fault();
static int dext_compute_quiescent();
static int dext_dma_quarantine_releasable();
static int dext_pci_quarantine_releasable();
static int dext_dma_lift_quarantine(int);
static int dext_pci_release_quarantine();
static int dext_bar0_cpu_release_orphaned();
static int dext_set_pci(void *, void *);
static int dext_open(uint32_t *);
static int dext_compute_query_info(uint64_t, uint64_t *, int);
static int dext_compute_release_client(uint64_t);
static bool dext_compute_client_owns(uint64_t);
static int dext_compute_forget_client(uint64_t);
static unsigned dext_compute_client_records();
static uint64_t legacyClientID;  // a client on the legacy (non-KFD) path
static bool dext_compute_client_legacy(uint64_t client) { return client && client == legacyClientID; }
// Surprise removal (rt/removal.h, dext_pci_* and dext_dma_device_removed).
struct pci_dev;
static bool devicePresent = true, dmaRemoved;
static int dext_pci_device_present() { return devicePresent; }
static void dext_pci_mark_removed() { events.push_back("pci_mark_removed"); }
static int dext_pci_close_removed();
static int rt_removal_begin(struct pci_dev *) { events.push_back("removal_begin"); return 0; }
static void rt_removal_end() { events.push_back("removal_end"); }
static void dext_compute_device_removed() { events.push_back("compute_removed"); }
// A definite transport fault made the GPU unreachable (rt/removal.h's
// rt_device_lost): its work completes with -ECANCELED from now on.
static int rt_device_lost_active(const char *) { events.push_back("device_lost"); return 0; }
static int dext_dma_device_removed() {
    events.push_back("dma_removed");
    // A device off the bus uses no mapping: retired descriptors complete.
    dmaRemoved = dmaCompleted = true;
    return 0;
}
struct rt_drm_info { int closes; };
static void IOSleep(uint64_t);
static void rt_drm_info_close(rt_drm_info *);
// Linux-file processes and CS self-tests (lx_files, cs_selftest): a session
// close tears them down first; a self-test whose GPU work never completed
// leaves the session quarantined, compute uncertain.
struct MacLinuxGPUUserClient;
static unsigned lxTeardowns, lxParked;
static void lx_gate_close() {}
static bool lx_teardown_all() {
    ++lxTeardowns;
    if (lxParked) events.push_back("lx_teardown_parked");
    return lxParked != 0;
}
static unsigned rt_cs_selftest_parked() { return lxParked; }
static void lx_client_stop(MacLinuxGPUUserClient *, IOService *) { assert(false); }
// A Linux-file client's process (rt/lx_files.h): taken and retired by its
// Stop's thread before lx_finish_stop runs; none left for it here.
struct rt_lx_client;
static rt_lx_client *lx_take(MacLinuxGPUUserClient *) { return nullptr; }
static int rt_lx_client_retire(rt_lx_client *, void (*)(void *), void *) { assert(false); return 0; }
// The display test (rt/display.h): a showing pattern is turned off while
// the driver runs, after observer admission drained, never in quarantine.
static bool displayShowing;
static int rt_display_showing() { return displayShowing; }
/* The write-pointer polling experiment is off (rt/wptr_poll.h). */
static bool rt_wptr_poll_active() { return false; }
static void rt_display_stop();
// A display agent's imported surfaces are released with the display.
static unsigned surfacesImported;
static unsigned rt_surface_count() { return surfacesImported; }
static unsigned rt_surface_remove_all();
// An observer's Stop releases what its display agent imported.
static unsigned observerDisplayStops;
static void observer_display_client_stop(uint64_t) { ++observerDisplayStops; }

// No KFD suspend is held in these scenarios (power_state.h): the hook that
// hands one back before upstream removal has nothing to do.
static unsigned powerRemovalHooks;
static void power_before_removal() { ++powerRemovalHooks; }

// The session's connectors leave the service's properties once the
// observer reads are closed (displays_publish runs only inside them).
static unsigned displayUnpublishes;
static void displays_unpublish(MacLinuxGPU *) { ++displayUnpublishes; }

#include <rt/recovery.h>
// GPU recovery comes off before upstream removal (counted, not an event).
static unsigned recoveryDetaches;
extern "C" void rt_recovery_detach_pdev(struct pci_dev *) { ++recoveryDetaches; }
static void session_client_stop(MacLinuxGPUUserClient *client, IOService *provider);
static void driver_stop(MacLinuxGPU *driver, IOService *provider);
// The driver's Stop starts a watch for a session call that never returns
// (session_watchdog_step is the production check, tested directly).
static unsigned watchdogStarts;
static void session_watchdog_start(MacLinuxGPU *, const char *, bool) { ++watchdogStarts; }
#include "session_shutdown_production.inc"
// Hooked in by InitDevice, which these scenarios do not run.
[[maybe_unused]] static void (*const recoveryNotify)(const struct rt_recovery_state *) = recovery_notify;

// An observer read in flight when the session closes: the close waits, and
// the read finishes (leaves) while the close sleeps.
static bool observerReadInFlight;
static void IOSleep(uint64_t ms) {
    assert(ms == 1 && !s_observerReads.admitting() && observerReadInFlight);
    events.push_back("observer_wait");
    observerReadInFlight = false;
    s_observerReads.leave();
}
static void rt_drm_info_close(rt_drm_info *drm) {
    assert(!s_observerReads.admitting() && s_observerReads.drained());
    assert(s_modulesRunning && !s_dmaQuarantined && !s_observerDrm);
    ++drm->closes;
    events.push_back("observer_drm_close");
}

static void rt_display_stop() {
    assert(displayShowing && !s_observerReads.admitting() && s_observerReads.drained());
    // Before Linux-file teardown in a close; for a removed device also from
    // the release of a session that was quarantined first.
    assert(s_modulesRunning && !s_dmaQuarantined && (lxTeardowns == 0 || s_deviceRemoved));
    events.push_back(s_deviceRemoved ? "display_off_removed" : "display_off");
    displayShowing = false;
}

static unsigned rt_surface_remove_all() {
    assert(surfacesImported && !s_observerReads.admitting() && s_observerReads.drained());
    assert(s_modulesRunning && !s_dmaQuarantined && !displayShowing &&
           (lxTeardowns == 0 || s_deviceRemoved));
    events.push_back("surfaces_release");
    const unsigned n = surfacesImported;
    surfacesImported = 0;
    return n;
}

static void *rt_device_get_pdev(void *device) {
    assert(device && device == s_rtDevice);
    return device;
}
static int rt_pci_probe_cleanup_retained(void *device) {
    assert(device && device == s_rtDevice);
    return probeCleanupRetained;
}

// What the device has mapped when the hold starts; the hold has no ceiling,
// so any amount is held (the DART alone refuses what does not fit).
static uint64_t mappedBytes = 64ull << 20, heldBytes;
static int dext_dma_begin_shutdown() {
    assert(s_irqDeliver && s_irqReady);
    events.push_back("hold_dma");
    if (!holdError) heldBytes = mappedBytes;
    return holdError;
}
static int dext_compute_stop() {
    assert(s_deviceRemoved || (s_irqDeliver && s_irqReady && s_dmaShutdownPrepared));
    events.push_back("compute_stop");
    if (!computeError) computeQuiescent = true;
    else computeQuiescent = false;
    return computeError;
}
static void linuxu_driver_shutdown() {
    assert(s_modulesRunning);
    assert(s_deviceRemoved || (s_irqDeliver && s_irqReady && s_dmaShutdownPrepared));
    events.push_back("upstream_shutdown");
    // amdgpu_pci_remove() unplugs the DRM device first, so the aperture
    // mapping survives the removal.
}
static void dext_dma_quarantine() { events.push_back("dma_quarantine"); }
static int dext_pci_quarantine() {
    events.push_back("pci_quarantine");
    if (transportFault == DEXT_PCI_FAULT_NONE) transportFault = DEXT_PCI_FAULT_QUARANTINE;
    return isolationError;
}
static int dext_irq_fini_async(void (*callback)(void *), void *context) {
    assert(!s_irqDeliver && !s_irqReady && !irqCompletion);
    events.push_back("cancel_irqs");
    irqCompletion = callback; irqContext = context;
    return irqError;
}
static void rt_device_free(void *device) {
    assert(device && device == s_rtDevice && irqDrained);
    assert(!s_modulesRunning && !s_irqDeliver);
    if (!s_deviceRemoved) assert(s_dmaShutdownPrepared && !endpointReset && !dmaCompleted);
    events.push_back("device_free");
}
static int dext_bar0_cpu_release_orphaned() {
    // Only after every Linux owner is gone; the real seam also requires the
    // shutdown hold and no other DMA owner or CPU alias.
    assert(irqDrained && !s_rtDevice && !s_modulesRunning && !endpointReset);
    const int released = bar0Aliases;
    if (released) events.push_back("release_bar0");
    bar0Aliases = 0;
    return released;
}
static int dext_pci_shutdown_reset() {
    assert(irqDrained && !s_irqDeliver && !s_rtDevice && !s_modulesRunning);
    assert(s_dmaShutdownPrepared && !dmaCompleted);
    events.push_back("endpoint_reset");
    // The real DMA seam refuses the reset while a BAR0 CPU mapping exists.
    if (bar0Aliases) return -16;
    if (resetError && resetFailures) {
        if (resetFailures != UINT32_MAX) --resetFailures;
        return resetError;
    }
    endpointReset = true;
    // This seam releases held DMA descriptors only after endpoint isolation.
    events.push_back("complete_dma");
    dmaCompleted = true;
    return 0;
}
static bool pciOpenExpected;
static void dext_compute_set_pci_open(bool open) { assert(open == pciOpenExpected); }
static void dext_compute_set_stage(int stage) { assert(stage == DEXT_COMPUTE_STAGE_NONE); }
static int dext_dma_fini() {
    assert(irqDrained && dmaCompleted && !s_rtDevice);
    // A removed device is never reset; its descriptors completed on removal.
    assert(s_deviceRemoved ? dmaRemoved && !endpointReset : endpointReset);
    events.push_back("dma_fini");
    return dmaFiniError;
}
static void dext_close() {
    assert(dmaCompleted && !dmaFiniError && !s_dmaQuarantined);
    events.push_back("pci_close");
}
static void rt_gart_reset() {
    assert(dmaCompleted && !s_dmaQuarantined);
    events.push_back("gart_reset");
}
static int dext_pci_transport_fault() { return transportFault; }
static int dext_compute_quiescent() { return computeQuiescent; }
static int dext_dma_quarantine_releasable() { return !dmaFiniError || endpointReset; }
static int dext_pci_quarantine_releasable() { return transportFault == DEXT_PCI_FAULT_QUARANTINE; }
static int dext_dma_lift_quarantine(int keep) {
    assert(keep && s_dmaQuarantined && s_finalCleanup);
    events.push_back("lift_dma_quarantine");
    // A retried release resets and completes the retained descriptors again.
    endpointReset = dmaCompleted = false;
    dmaFiniError = 0;
    return 0;
}
static int dext_pci_release_quarantine() {
    events.push_back("reopen_pci");
    if (transportFault != DEXT_PCI_FAULT_QUARANTINE) return -5;
    transportFault = DEXT_PCI_FAULT_NONE;
    return 0;
}
static int dext_set_pci(void *pci, void *client) {
    assert(pci == s_retainedPCI && client == s_driver && !s_pciOpen);
    events.push_back("pci_open");
    return 0;
}
static int dext_open(uint32_t *token) { ++pciOpens; *token = 9; return 0; }
static int dext_compute_query_info(uint64_t tag, uint64_t *out, int) {
    assert(tag == 4); out[0] = 2; return 1;
}
static int releaseError;
static std::vector<uint64_t> releasedClients;
// The compute backend's client records (dext_compute_backend.inc): a fixed
// table of 64, one per client given an identity (or asking for a compute
// session); what a client owns (a KFD process, queues, BOs) goes with its
// release, the record with it; forgetting keeps a record whose KFD process
// is not closed. The real table's stop and churn are dext_compute
// production checks; here, that every Stop path lets its record go.
static constexpr size_t kClientRecords = 64;
static std::vector<uint64_t> records, owners, keptKFD;
static bool has(const std::vector<uint64_t> &set, uint64_t client) {
    return std::find(set.begin(), set.end(), client) != set.end();
}
static void erase(std::vector<uint64_t> &set, uint64_t client) {
    set.erase(std::remove(set.begin(), set.end(), client), set.end());
}
// A client's first session call (record_client_identity), or a QueryInfo
// that opens its KFD process: a record, which the table must have room for.
static void recordClient(uint64_t client, bool owns) {
    assert(!has(records, client));
    assert(records.size() < kClientRecords && "every compute record is held");
    records.push_back(client);
    if (owns) owners.push_back(client);
}
[[maybe_unused]] static bool dext_compute_client_owns(uint64_t client) { return has(owners, client); }
[[maybe_unused]] static int dext_compute_forget_client(uint64_t client) {
    if (has(keptKFD, client)) return -16;
    erase(records, client);
    return 0;
}
[[maybe_unused]] static unsigned dext_compute_client_records() { return unsigned(records.size()); }
static int dext_compute_release_client(uint64_t client) {
    events.push_back("release_client");
    releasedClients.push_back(client);
    // A client's KFD close fails as the compute stop would (computeError).
    const int r = releaseError ? releaseError : (computeError ? -16 : 0);
    if (!r) { erase(owners, client); erase(records, client); }
    else if (has(owners, client)) keptKFD.push_back(client);
    return r;
}

static int dext_pci_close_removed() {
    // Nothing left that could touch the device: interrupts drained,
    // upstream removed, runtime device freed, DMA released.
    assert(irqDrained && !s_modulesRunning && !s_rtDevice && dmaRemoved && !endpointReset);
    events.push_back("pci_close_removed");
    return 0;
}
static void power_device_removed() { events.push_back("power_lost"); }

void MacLinuxGPU::FinishStop(IOService *provider) {
    assert(!s_dmaQuarantined && !s_sessionClosing && dmaCompleted);
    events.push_back("super_driver_stop"); ++driverStops;
    provider->release(); release();
}
void MacLinuxGPUUserClient::FinishStop(IOService *provider) {
    // A client finishes once the session that it ended is closed, or, the
    // device staying up across clients, with the session still open.
    if (!ivars->observer)
        assert(!s_dmaQuarantined && !s_sessionClosing && (dmaCompleted || s_pciOpen));
    events.push_back(ivars->observer ? "observer_stop" : "super_client_stop");
    if (!ivars->observer) ++clientStops;
    auto owner = ivars->ownerDriver;
    s_rawBARLease.release(ivars->clientID);
    ivars = nullptr;
    Stop(provider, SUPERDISPATCH);
    provider->release(); owner->release(); release();
}

struct Fixture {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient clients[2], observer;
    MacLinuxGPUUserClient_IVars ivars[3]{};
    unsigned retainedDriver, retainedProvider;
    int device;
    Fixture() {
        s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
        s_rtDevice = &device; s_modulesRunning = true;
        s_irqReady = s_irqDeliver = s_pciOpen = true;
        s_token = 7; s_stopProvider = &provider;
        bar0Aliases = 1;
        // Stop has retained the driver/provider; each stopping client also
        // retains its provider, original owning driver, and itself.
        driver.retain(); provider.retain();
        for (unsigned i = 0; i < 2; i++) {
            clients[i].ivars = &ivars[i];
            ivars[i].ownerDriver = &driver; ivars[i].stopProvider = &driver;
            ivars[i].clientID = i + 1; ivars[i].stopping = true;
            ivars[i].ownerQueue = &s_ownerQueueAtOnce;
            ivars[i].nextStopping = i ? nullptr : &clients[1];
            driver.retain(); driver.retain(); clients[i].retain();
        }
        s_stoppingClients = &clients[0];
        observer.ivars = &ivars[2];
        ivars[2].ownerDriver = &driver; ivars[2].clientID = 3;
        ivars[2].ownerQueue = &s_ownerQueueAtOnce;
        retainedDriver = driver.references; retainedProvider = provider.references;
    }
    void deliverIRQDrain() {
        assert(irqCompletion && queue.pending.empty());
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        assert(queue.pending.size() == 1);
        queue.drain();
    }
    void assertRetained(bool missingCallback) {
        assert(s_dmaQuarantined && s_sessionClosing && s_quarantineRetained);
        assert(!clientStops && !driverStops && s_stopProvider == &provider);
        assert(s_stoppingClients == &clients[0] && s_retainedPCI == &provider);
        assert(clients[0].ivars == &ivars[0] && clients[1].ivars == &ivars[1]);
        assert(clients[0].references == 2 && clients[1].references == 2);
        assert(provider.references == retainedProvider);
        assert(driver.references == retainedDriver + 1 + unsigned(missingCallback));
        assert(s_pciOpen && s_token == 7 && s_sessionGeneration == 1);
    }
    void assertReleased() {
        assert(clientStops == 2 && driverStops == 1);
        assert(!s_pciOpen && !s_sessionClosing && !s_dmaShutdownPrepared && !s_token);
        assert(!s_dmaQuarantined && !s_quarantineRetained);
        assert(!s_stoppingClients && !s_stopProvider && s_sessionGeneration == 2);
        assert(driver.references == 1 && provider.references == 1);
    }
};

static bool saw(const char *event) {
    for (const auto &entry : events) if (entry == event) return true;
    return false;
}
static std::vector<uint64_t> state() {
    std::vector<uint64_t> out(MLG_SESSION_STATE_WORDS);
    session_state(out.data());
    assert(out[0] == MLG_SESSION_STATE_VERSION);
    return out;
}
static void checkSelectorGuards(Fixture &fixture) {
    uint64_t output[2]{};
    IOUserClientMethodArguments arguments{0, output, 2};
    fixture.observer.ivars->sessionGeneration = s_sessionGeneration;
    s_participants = 2;
    assert(fixture.observer.shutdown(&arguments) == kIOReturnBusy && events.empty());
    s_participants = 1;
    assert(s_rawBARLease.claim(3, true, 1) && s_rawBARLease.markMapped(3));
    assert(fixture.observer.shutdown(&arguments) == kIOReturnBusy && events.empty());
    s_rawBARLease.release(3);
    assert(fixture.observer.shutdown(&arguments) == kIOReturnSuccess);
    assert(output[0] == kIOReturnBusy && output[1] == 2);
    auto count = events.size();
    assert(fixture.observer.shutdown(&arguments) == kIOReturnSuccess);
    assert(output[0] == kIOReturnBusy && output[1] == 2 && events.size() == count);
    fixture.deliverIRQDrain();
    assert(fixture.observer.shutdown(&arguments) == kIOReturnSuccess);
    assert(output[0] == kIOReturnSuccess && output[1] == 6);
}

// The observer policy reads cached state only and never opens a session.
static void checkObserverPolicy() {
    const uint64_t probe[] = {MLG_QUERY_PROBE_STATUS};
    const uint64_t session[] = {MLG_QUERY_SESSION_STATE};
    const uint64_t log[] = {MLG_QUERY_KERNEL_LOG, 0};
    const uint64_t topology[] = {10};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_PING, nullptr, 0));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RUNTIME_BUILD, nullptr, 0));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RELEASE_QUARANTINE, nullptr, 0));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, probe, 1));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, session, 1));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, log, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, log, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, probe, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, topology, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, nullptr, 0));
    // The power state, and the power selector's ops (PREPARE and RESUME
    // check the release entitlement in the handler).
    const uint64_t power[] = {MLG_QUERY_POWER_STATE};
    const uint64_t query[] = {MLG_POWER_OP_QUERY}, wait[] = {MLG_POWER_OP_WAIT, 3};
    const uint64_t prepare[] = {MLG_POWER_OP_PREPARE}, badOp[] = {MLG_POWER_OP_WAIT + 1};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, power, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_QUERY_INFO, power, 2));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_POWER, query, 1));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_POWER, wait, 2));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_POWER, prepare, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_POWER, badOp, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_POWER, nullptr, 0));
    const uint64_t read[] = {MLG_SYSFS_OP_READ, 0}, list[] = {MLG_SYSFS_OP_LIST, 4096};
    const uint64_t write[] = {2, 0}, info[] = {0x1d, 4}, huge[] = {0x1d, MLG_SYSFS_CHUNK_MAX + 1};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, read, 2));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, list, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, write, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, read, 1));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, info, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, huge, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, info, 1));
    // SysfsWrite: an allowlisted attribute and a value size within bounds
    // (the value itself is checked in the handler);
    // synchronous, like the bounded reads.
    const uint64_t perf[] = {MLG_SYSFS_WRITE_PERF_LEVEL, 4}, profile[] = {MLG_SYSFS_WRITE_POWER_PROFILE, 1};
    const uint64_t badAttr[] = {MLG_SYSFS_WRITE_ATTRS, 4}, empty[] = {MLG_SYSFS_WRITE_PERF_LEVEL, 0};
    const uint64_t long_[] = {MLG_SYSFS_WRITE_PERF_LEVEL, MLG_SYSFS_WRITE_VALUE_MAX + 1};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, perf, 2));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, profile, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, badAttr, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, empty, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, long_, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_WRITE, perf, 1));
    assert(mlg_call_is_synchronous(MLG_SELECTOR_SYSFS_WRITE, perf, 2));
    assert(!mlg_call_runs_on_delivery(MLG_SELECTOR_SYSFS_WRITE, perf, 2));
    assert(!strcmp(mlg_sysfs_write_path(MLG_SYSFS_WRITE_PERF_LEVEL), "power_dpm_force_performance_level"));
    assert(!strcmp(mlg_sysfs_write_path(MLG_SYSFS_WRITE_POWER_PROFILE), "pp_power_profile_mode"));
    assert(!mlg_sysfs_write_path(MLG_SYSFS_WRITE_ATTRS));
    {
        char value[MLG_SYSFS_WRITE_VALUE_MAX + 1];
        const auto ok = [&](uint64_t attr, const char *v) { return mlg_sysfs_write_value(attr, v, strlen(v), value); };
        for (const char *level : {"auto", "low", "high", "profile_peak", "high\n"})
            assert(ok(MLG_SYSFS_WRITE_PERF_LEVEL, level));
        assert(ok(MLG_SYSFS_WRITE_PERF_LEVEL, "high\n") && !strcmp(value, "high"));
        for (const char *level : {"manual", "profile_standard", "profile_min_sclk", "hig", "highx",
                                  "auto\n\n", "\n", "AUTO", "high 1", "peak"})
            assert(!ok(MLG_SYSFS_WRITE_PERF_LEVEL, level));
        for (const char *index : {"0", "5", "15", "99", "7\n"})
            assert(ok(MLG_SYSFS_WRITE_POWER_PROFILE, index));
        for (const char *index : {"100", "-1", "5 1 2", "a", "", "6 0 1 2 3 4 5 6 7 8"})
            assert(!ok(MLG_SYSFS_WRITE_POWER_PROFILE, index));
        assert(!mlg_sysfs_write_value(MLG_SYSFS_WRITE_ATTRS, "1", 1, value));
        assert(!mlg_sysfs_write_value(MLG_SYSFS_WRITE_PERF_LEVEL, nullptr, 4, value));
    }
    // Display: an explicit confirmation word, a known op, a pattern only
    // for SHOW.
    const uint64_t displayProbe[] = {MLG_DISPLAY_OP_PROBE, 0, MLG_DISPLAY_CONFIRM};
    const uint64_t displayShow[] = {MLG_DISPLAY_OP_SHOW, MLG_DISPLAY_PATTERN_GRADIENT, MLG_DISPLAY_CONFIRM};
    const uint64_t displayOff[] = {MLG_DISPLAY_OP_OFF, 0, MLG_DISPLAY_CONFIRM};
    const uint64_t displayNoConfirm[] = {MLG_DISPLAY_OP_SHOW, 0, 0};
    const uint64_t displayBadPattern[] = {MLG_DISPLAY_OP_SHOW, MLG_DISPLAY_PATTERNS, MLG_DISPLAY_CONFIRM};
    const uint64_t displayOffPattern[] = {MLG_DISPLAY_OP_OFF, 1, MLG_DISPLAY_CONFIRM};
    const uint64_t displayStatus[] = {MLG_DISPLAY_OP_STATUS, 0, MLG_DISPLAY_CONFIRM};
    const uint64_t displayModes[] = {MLG_DISPLAY_OP_MODES, 0, MLG_DISPLAY_CONFIRM};
    const uint64_t displayModesPattern[] = {MLG_DISPLAY_OP_MODES, 1, MLG_DISPLAY_CONFIRM};
    const uint64_t displayBadOp[] = {10, 0, MLG_DISPLAY_CONFIRM};
    const uint64_t displayImport[] = {MLG_DISPLAY_OP_IMPORT, (2560ULL << 48) | (1440ULL << 32) | 10240,
                                      MLG_DISPLAY_CONFIRM};
    const uint64_t displayImportBad[] = {MLG_DISPLAY_OP_IMPORT, (1440ULL << 32) | 10240, MLG_DISPLAY_CONFIRM};
    const uint64_t displayVerify[] = {MLG_DISPLAY_OP_VERIFY, (3ULL << 32) | 7, MLG_DISPLAY_CONFIRM};
    const uint64_t displayVerifyBad[] = {MLG_DISPLAY_OP_VERIFY, 7, MLG_DISPLAY_CONFIRM};
    const uint64_t displayPresent[] = {MLG_DISPLAY_OP_PRESENT, 3, MLG_DISPLAY_CONFIRM};
    const uint64_t displayPresentBad[] = {MLG_DISPLAY_OP_PRESENT, 1ULL << 32, MLG_DISPLAY_CONFIRM};
    const uint64_t displayOutput[] = {MLG_DISPLAY_OP_OUTPUT, 59951, MLG_DISPLAY_CONFIRM};
    const uint64_t displayOutputBad[] = {MLG_DISPLAY_OP_OUTPUT, 0, MLG_DISPLAY_CONFIRM};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayImport, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayImportBad, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayVerify, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayVerifyBad, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayPresent, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayPresentBad, 3));
    {
        /* Handle 0: PRESENT with no rectangle reads the statistics. */
        const uint64_t displayStats[] = {MLG_DISPLAY_OP_PRESENT, 0, MLG_DISPLAY_CONFIRM};
        assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayStats, 3));
    }
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayOutput, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayOutputBad, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayStatus, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayModes, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayModesPattern, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayProbe, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayShow, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayOff, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayShow, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayNoConfirm, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayBadPattern, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayOffPattern, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, displayBadOp, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DISPLAY, nullptr, 0));
    // Retire: a known op, only the FORCE flag, the confirmation word.
    const uint64_t retire[] = {MLG_RETIRE_OP_TERMINATE, MLG_RETIRE_FORCE, MLG_RETIRE_CONFIRM};
    const uint64_t retireQuiesce[] = {MLG_RETIRE_OP_QUIESCE, 0, MLG_RETIRE_CONFIRM};
    const uint64_t retireResume[] = {MLG_RETIRE_OP_RESUME, 0, MLG_RETIRE_CONFIRM};
    const uint64_t retireBadOp[] = {MLG_RETIRE_OP_DISCONNECT + 1, 0, MLG_RETIRE_CONFIRM};
    const uint64_t retireDisconnect[] = {MLG_RETIRE_OP_DISCONNECT, 0, MLG_RETIRE_CONFIRM};
    const uint64_t retireBadFlags[] = {MLG_RETIRE_OP_QUIESCE, 2, MLG_RETIRE_CONFIRM};
    const uint64_t retireNoConfirm[] = {MLG_RETIRE_OP_TERMINATE, 0, 0};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retire, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireQuiesce, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireResume, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retire, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireBadOp, 3));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireDisconnect, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireBadFlags, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, retireNoConfirm, 3));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_RETIRE, nullptr, 0));
    char path[MLG_SYSFS_PATH_MAX + 1];
    assert(mlg_sysfs_path_copy(path, "gpu_metrics", 11, false) && !std::strcmp(path, "gpu_metrics"));
    assert(mlg_sysfs_path_copy(path, "hwmon/hwmon0/temp1_input", 25, false) &&
           !std::strcmp(path, "hwmon/hwmon0/temp1_input"));
    for (const char *bad : {"", "/gpu_metrics", "hwmon/", "a//b", "..", "../x", "a/../b", "./a", "a/."})
        assert(!mlg_sysfs_path_copy(path, bad, std::strlen(bad), false));
    assert(!mlg_sysfs_path_copy(path, "a\0b", 3, false) && !mlg_sysfs_path_copy(path, nullptr, 0, false));
    // Only a listing may name the device directory itself, with no path.
    assert(mlg_sysfs_path_copy(path, nullptr, 0, true) && !path[0]);
    assert(mlg_sysfs_path_copy(path, "", 1, true) && !path[0]);
    assert(!mlg_sysfs_path_copy(path, "..", 2, true) && !mlg_sysfs_path_copy(path, "/", 1, true));
    const std::string longest(MLG_SYSFS_PATH_MAX, 'a'), tooLong(MLG_SYSFS_PATH_MAX + 1, 'a');
    assert(mlg_sysfs_path_copy(path, longest.c_str(), longest.size() + 1, false));
    assert(!mlg_sysfs_path_copy(path, tooLong.c_str(), tooLong.size(), false));
    for (uint64_t selector = 1; selector < 128; ++selector) {
        if (selector == MLG_SELECTOR_QUERY_INFO || selector == MLG_SELECTOR_RUNTIME_BUILD ||
            selector == MLG_SELECTOR_RELEASE_QUARANTINE || selector == MLG_SELECTOR_OWNER_RESULT)
            continue;
        assert(!mlg_observer_selector_allowed(selector, probe, 1));
    }
    // OWNER_RESULT names the call's token, never 0.
    const uint64_t token[] = {5}, noToken[] = {0};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_OWNER_RESULT, token, 1));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_OWNER_RESULT, noToken, 1));

    // What runs on the delivery thread (session_state.h): cached state,
    // the power query and wait, the result fetch; nothing else.
    const uint64_t klog[] = {MLG_QUERY_KERNEL_LOG, 0}, qSession[] = {MLG_QUERY_SESSION_STATE},
                   qPower[] = {MLG_QUERY_POWER_STATE}, probeStatus[] = {MLG_QUERY_PROBE_STATUS},
                   computeTag[] = {10}, powerQuery[] = {MLG_POWER_OP_QUERY},
                   powerWait[] = {MLG_POWER_OP_WAIT, 1}, powerPrepare[] = {MLG_POWER_OP_PREPARE};
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_PING, nullptr, 0));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_RUNTIME_BUILD, nullptr, 0));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, klog, 2));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, qSession, 1));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, qPower, 1));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, probeStatus, 1));
    assert(!mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, computeTag, 1));
    assert(!mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, klog, 1));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_POWER, powerQuery, 1));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_POWER, powerWait, 2));
    assert(!mlg_call_runs_on_delivery(MLG_SELECTOR_POWER, powerPrepare, 1));
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_OWNER_RESULT, token, 1));
    assert(!mlg_call_runs_on_delivery(MLG_SELECTOR_OWNER_RESULT, noToken, 1));
    for (uint64_t selector : {1ull, 2ull, 6ull, 9ull, 16ull, 17ull, 36ull, 49ull, 50ull, 51ull, 54ull,
                              55ull, 56ull, 57ull, 58ull, 59ull, 61ull, 82ull, 85ull, 86ull, 87ull})
        assert(!mlg_call_runs_on_delivery(selector, probe, 1) &&
               !mlg_call_is_synchronous(selector, probe, 1));
    // The bounded reads and the display's PRESENT and RESULT stay
    // synchronous; its other ops are async.
    const uint64_t present[] = {MLG_DISPLAY_OP_PRESENT, 0, MLG_DISPLAY_CONFIRM},
                   result[] = {MLG_DISPLAY_OP_RESULT, 1, MLG_DISPLAY_CONFIRM},
                   output[] = {MLG_DISPLAY_OP_OUTPUT, 1, MLG_DISPLAY_CONFIRM};
    assert(mlg_call_is_synchronous(MLG_SELECTOR_SYSFS_READ, probe, 1));
    assert(mlg_call_is_synchronous(MLG_SELECTOR_DRM_INFO, probe, 1));
    assert(mlg_call_is_synchronous(MLG_SELECTOR_DISPLAY, present, 3));
    assert(mlg_call_is_synchronous(MLG_SELECTOR_DISPLAY, result, 3));
    assert(!mlg_call_is_synchronous(MLG_SELECTOR_DISPLAY, output, 3));
}

// The device stays up across clients (Linux keeps amdgpu probed while
// programs open and close their files). Successful probe and compute start,
// then the only client exits without destroying anything: it releases what
// it owned and finishes; nothing closes, and the next client joins the same
// session without opening PCI or probing again.
static void clientExitReopen(bool queueExhausted) {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client, next;
    MacLinuxGPUUserClient_IVars clientIvars{}, nextIvars{};
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    client.ivars = &clientIvars;
    clientIvars = {&driver, nullptr, nullptr, s_sessionGeneration, 1, false, false, false, &s_ownerQueueAtOnce, nullptr,
                   nullptr, 0, nullptr, false, false, nullptr, nullptr, 0, nullptr, 0, false};
    driver.retain(); s_participants = 1;
    // A second queue found every slot held; the client's release covers
    // what it had.
    if (queueExhausted) computeError = 0;
    const uint64_t generation = s_sessionGeneration;
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(!s_sessionClosing && s_participants == 0 && clientStops == 1 && client.superStops == 1);
    assert(events == std::vector<std::string>({"release_client", "super_client_stop"}));
    assert(releasedClients == std::vector<uint64_t>({1}));
    assert(s_pciOpen && s_modulesRunning && s_rtDevice == &device && s_token == 7 && !irqCompletion);
    assert(s_sessionGeneration == generation && powerRemovalHooks == 0 && displayUnpublishes == 0);
    assert(!s_dmaQuarantined && driver.references == 1);
    const auto snapshot = state();
    assert(!(snapshot[1] & (MLG_SESSION_FLAG_QUARANTINED | MLG_SESSION_FLAG_CLOSING)));
    assert(snapshot[1] & MLG_SESSION_FLAG_MODULES_RUNNING);
    // The next client joins the running device: no PCI open, no probe.
    next.ivars = &nextIvars;
    nextIvars = {&driver, nullptr, nullptr, 0, 2, false, false, false, &s_ownerQueueAtOnce, nullptr,
                 nullptr, 0, nullptr, false, false, nullptr, nullptr, 0, nullptr, 0, false};
    assert(ensure_open(&next) == kIOReturnSuccess);
    assert(s_pciOpen && s_participants == 1 && pciOpens == 0 && !saw("pci_open"));
    assert(nextIvars.sessionGeneration == s_sessionGeneration);
    std::printf("PASS production session shutdown: %s\n",
                queueExhausted ? "queue-exhaustion-exit" : "client-exit-reopen");
}

// The device's real close points still close it when the last client
// leaves: a session that never brought the GPU up (what was set before the
// probe was that client's), a release that failed (tainted), a raw BAR
// mapping nothing proves revoked, and a client on the legacy (non-KFD) path
// (the iPad's Studio: its host window is the GART window in its address
// space, so its next launch places a window again).
static void clientExitCloses(const std::string &kind) {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client;
    MacLinuxGPUUserClient_IVars clientIvars{};
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    const bool probed = kind != "client-exit-unprobed";
    if (kind == "client-exit-legacy") legacyClientID = 1;
    s_rtDevice = probed ? &device : nullptr; s_modulesRunning = probed; s_probeAttempted = probed;
    s_irqReady = s_irqDeliver = true; s_pciOpen = true; s_token = 7;
    bar0Aliases = probed ? 1 : 0;
    client.ivars = &clientIvars;
    clientIvars = {&driver, nullptr, nullptr, s_sessionGeneration, 1, false, false, false, &s_ownerQueueAtOnce, nullptr,
                   nullptr, 0, nullptr, false, false, nullptr, nullptr, 0, nullptr, 0, false};
    driver.retain(); s_participants = 1;
    if (kind == "client-exit-release-failure") releaseError = -16;
    if (kind == "client-exit-raw-mapped") {
        assert(s_rawBARLease.claim(1, true, 1));
        (void)s_rawBARLease.markMapped(1);
    }
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(s_sessionClosing && s_participants == 0 && !clientStops);
    // The client's release runs unless the close takes everything at once.
    assert(saw("release_client") == (kind == "client-exit-release-failure"));
    if (kind == "client-exit-legacy") assert(saw("upstream_shutdown"));
    if (kind == "client-exit-unprobed") assert(!saw("upstream_shutdown"));
    if (kind == "client-exit-release-failure" || kind == "client-exit-raw-mapped") {
        // Tainted: the close keeps what it cannot prove released.
        assert(s_dmaQuarantined);
        std::printf("PASS production session shutdown: %s\n", kind.c_str());
        return;
    }
    if (irqCompletion) {
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
    }
    queue.drain();
    assert(!s_sessionClosing && !s_pciOpen && clientStops == 1 && saw("pci_close"));
    std::printf("PASS production session shutdown: %s\n", kind.c_str());
}

// Two to four programs at once, as on Linux: HSA session clients and
// Linux-file (RADV) clients join the running device, work, and leave in
// overlapping orders; one leaving never closes the device under the others
// or blocks one that joins meanwhile. An observer (the display agent's
// reader) comes and goes throughout. The device closes only at the driver's
// Stop, with every Stop finished.
static void concurrentClients() {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    const uint64_t generation = s_sessionGeneration;
    struct Program {
        MacLinuxGPUUserClient client;
        MacLinuxGPUUserClient_IVars ivars{};
        bool joined = false;
    };
    static Program programs[8];
    unsigned next = 0;
    const auto start = [&](bool linuxFile) -> Program & {
        Program &p = programs[next];
        p.ivars = {};
        p.ivars.ownerDriver = &driver;
        p.ivars.clientID = 10 + next++;
        p.ivars.ownerQueue = &s_ownerQueueAtOnce;
        p.ivars.linuxFile = linuxFile;
        p.client.ivars = &p.ivars;
        driver.retain();
        assert(ensure_open(&p.client) == kIOReturnSuccess);
        p.joined = true;
        return p;
    };
    const auto leave = [&](Program &p) {
        const unsigned before = s_participants, stops = clientStops;
        if (p.ivars.linuxFile) {
            // Its Stop's thread retired its process; the membership ends on
            // the owner's queue.
            // (Its Stop retained the client and the provider.)
            p.ivars.stopping = true;
            p.client.retain(); driver.retain();
            lx_finish_stop(&p.client, &driver);
        } else {
            assert(p.client.Stop(&driver) == kIOReturnSuccess);
        }
        assert(s_participants == before - 1 && clientStops == stops + 1);
        assert(!s_sessionClosing && s_pciOpen && s_modulesRunning && s_sessionGeneration == generation);
        p.joined = false;
    };
    // The display agent's observer, attached throughout.
    MacLinuxGPUUserClient observer;
    MacLinuxGPUUserClient_IVars observerIvars{};
    observerIvars.ownerDriver = &driver; observerIvars.clientID = 99; observerIvars.observer = true;
    observerIvars.ownerQueue = &s_ownerQueueAtOnce;
    observer.ivars = &observerIvars;

    // Two HSA programs and a RADV program; the first HSA one leaves while
    // the others work and a fourth (RADV) joins; then the rest leave in a
    // different order than they came, and a new HSA program starts after
    // all of them left (no cold start: no PCI open, no probe).
    Program &hsa1 = start(false), &hsa2 = start(false), &radv1 = start(true);
    assert(s_participants == 3);
    leave(hsa1);
    Program &radv2 = start(true);
    assert(s_participants == 3);
    leave(radv1);
    leave(hsa2);
    leave(radv2);
    assert(s_participants == 0 && pciOpens == 0 && !saw("pci_open") && !saw("upstream_shutdown"));
    Program &hsa3 = start(false);
    assert(s_participants == 1 && pciOpens == 0);
    // Each HSA program released what it owned; Linux-file programs hold no
    // compute handles (their files closed with their process).
    assert(releasedClients == std::vector<uint64_t>({10, 11}));
    leave(hsa3);
    assert(releasedClients.size() == 3);

    // The join/leave soak: many programs, two at a time, overlapping.
    for (unsigned round = 0; round < 1000; ++round) {
        MacLinuxGPUUserClient a, b;
        MacLinuxGPUUserClient_IVars ai{}, bi{};
        ai.ownerDriver = bi.ownerDriver = &driver;
        ai.clientID = 1000 + 2 * round; bi.clientID = 1001 + 2 * round;
        ai.ownerQueue = bi.ownerQueue = &s_ownerQueueAtOnce;
        bi.linuxFile = round % 2;
        a.ivars = &ai; b.ivars = &bi;
        driver.retain(); driver.retain();
        assert(ensure_open(&a) == kIOReturnSuccess && ensure_open(&b) == kIOReturnSuccess);
        assert(s_participants == 2);
        assert(a.Stop(&driver) == kIOReturnSuccess);
        if (bi.linuxFile) { bi.stopping = true; b.retain(); driver.retain(); lx_finish_stop(&b, &driver); }
        else assert(b.Stop(&driver) == kIOReturnSuccess);
        assert(s_participants == 0 && !s_sessionClosing && s_sessionGeneration == generation);
    }
    assert(pciOpens == 0 && !saw("pci_open") && !saw("upstream_shutdown") && !s_dmaQuarantined);
    assert(clientStops == 5 + 2000 && driver.references == 1);

    // The driver's Stop closes the device, once.
    driver.retain(); provider.retain();
    assert(driver.Stop(&provider) == kIOReturnSuccess);
    assert(s_sessionClosing);
    auto callback = irqCompletion; auto context = irqContext;
    irqCompletion = nullptr; irqContext = nullptr;
    irqDrained = true; events.push_back("irq_drained");
    callback(context);
    queue.drain();
    assert(driverStops == 1 && !s_pciOpen && !s_sessionClosing && s_sessionGeneration == generation + 1);
    assert(std::count(events.begin(), events.end(), "upstream_shutdown") == 1);
    std::puts("PASS production concurrent clients: HSA and Linux-file programs join and leave in overlapping "
              "orders without closing the device or blocking each other; 1000 join/leave rounds of two at a "
              "time; the driver's Stop closes it once");
}

// Surprise removal: the GPU leaves the bus with a client's session open.
// The client is stopped first (IOKit terminates the clients, then the
// provider). With "quarantined", the session was already quarantined (the
// client's release failed while the device was still there, and the close
// kept every owner, the display's included) when the provider stop sees the
// device gone. Either way no reset, isolation or quarantine
// follows: everything is released, the provider closes, every Stop
// finishes, and the next client opens a new session.
static void surpriseRemoval(bool quarantined, bool held = false) {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client, next;
    MacLinuxGPUUserClient_IVars clientIvars{}, nextIvars{};
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    client.ivars = &clientIvars;
    clientIvars = {&driver, nullptr, nullptr, s_sessionGeneration, 1, false, false, false, &s_ownerQueueAtOnce, nullptr,
                   nullptr, 0, nullptr, false, false, nullptr, nullptr, 0, nullptr, 0, false};
    driver.retain(); s_participants = 1;
    // The KFD close cannot confirm anything once MES is gone.
    computeError = -11006;
    // A display agent was mirroring onto the GPU: an output on screen and
    // two imported capture surfaces.
    displayShowing = true;
    surfacesImported = 2;
    // "held": the session was quarantined before the close began, so the
    // close keeps the display's owners until the removal releases them.
    if (held) { s_dmaQuarantined = true; note_quarantine(MLG_QUARANTINE_COMPUTE_UNCERTAIN, -5); }
    if (!quarantined) devicePresent = false;
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(s_sessionClosing && s_participants == 0 && !clientStops);
    if (quarantined) {
        assert(s_dmaQuarantined && !s_deviceRemoved);
        assert(saw("pci_quarantine") && saw("dma_quarantine"));
        // Retained with every other owner while quarantined.
        assert(displayShowing && surfacesImported == 2 && !saw("display_off"));
    } else {
        assert(!s_dmaQuarantined && s_deviceRemoved);
    }
    {
        assert(irqCompletion);
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        queue.drain();
    }
    if (quarantined) {
        // Retained until the provider stops and finds the device gone. The
        // real Stop releases the session and the provider, and closes
        // nothing again: the old instance must go so a replug attaches a
        // fresh one (a second close would re-quarantine and keep it alive).
        assert(s_dmaQuarantined && !clientStops);
        devicePresent = false;
        const auto closes = [] {
            const auto text = retainedLog();
            size_t n = 0;
            for (size_t at = text.find("session close begin"); at != std::string::npos;
                 at = text.find("session close begin", at + 1)) ++n;
            return n;
        };
        const size_t before = closes();
        assert(driver.Stop(&provider) == kIOReturnSuccess);
        assert(driverStops == 1 && !s_stopProvider && closes() == before);
        assert(!s_dmaQuarantined && !s_sessionClosing);
        s_stopping = false;	/* the replug's instance is a fresh process */
    }
    assert(!s_dmaQuarantined && !s_sessionClosing && !s_quarantineRetained && !s_deviceRemoved);
    assert(clientStops == 1 && client.superStops == 1);
    assert(driver.references == 1 && provider.references == 1 && s_sessionGeneration == 2);
    assert(saw("pci_mark_removed") && saw("removal_begin") && saw("compute_removed"));
    assert(saw("dma_removed") && saw("power_lost") && saw("removal_end"));
    assert(saw("upstream_shutdown") && saw("device_free") && saw("pci_close_removed"));
    assert(!saw("endpoint_reset") && !saw("pci_close"));
    // The display's owners are gone before upstream removal; for a device
    // already known removed nothing was committed.
    assert(!displayShowing && !surfacesImported);
    {
        auto at = [](const char *e) { return std::find(events.begin(), events.end(), e) - events.begin(); };
        const char *off = "display_off_removed";
        assert(at(off) < at("surfaces_release") && at("surfaces_release") < at("upstream_shutdown"));
        assert(at("removal_begin") < at(off) && !saw("display_off"));
    }
    expectLog("released 2 imported surface(s)");
    if (!quarantined) {
        const std::vector<std::string> expected{
            "pci_mark_removed", "removal_begin", "compute_removed", "dma_removed", "power_lost",
            "display_off_removed", "surfaces_release", "compute_stop", "removal_end", "upstream_shutdown", "cancel_irqs",
            "irq_drained", "enqueue_finish", "device_free", "release_bar0", "dma_removed",
            "dma_fini", "pci_close_removed", "gart_reset", "super_client_stop"};
        assert(events == expected);
        assert(!saw("pci_quarantine") && !saw("hold_dma"));
    }
    const auto snapshot = state();
    assert(!(snapshot[1] & (MLG_SESSION_FLAG_QUARANTINED | MLG_SESSION_FLAG_CLOSING |
                            MLG_SESSION_FLAG_DEVICE_REMOVED)));
    expectLog("device removed from the bus");
    // The replugged device: a new client opens a new session.
    devicePresent = true;
    next.ivars = &nextIvars;
    nextIvars = {&driver, nullptr, nullptr, 0, 2, false, false, false, &s_ownerQueueAtOnce, nullptr,
                 nullptr, 0, nullptr, false, false, nullptr, nullptr, 0, nullptr, 0, false};
    pciOpenExpected = true;
    assert(ensure_open(&next) == kIOReturnSuccess);
    assert(s_pciOpen && s_participants == 1 && saw("pci_open"));
    std::printf("PASS production session shutdown: %s\n",
                held ? "surprise-removal-held" : quarantined ? "surprise-removal-quarantined" : "surprise-removal");
}

// ----------------------------------------------------------------
// Driver upgrade. macOS does not stop a running dext when an activation
// request replaces it: the replacement attaches only once every instance of
// the old one is gone. The installer sends Retire (TERMINATE) after the
// replacement was accepted; IOKit then stops the clients and the driver.
// These run the production Stop and Retire paths and require that each one
// finishes every Stop and releases the service and provider, without a
// quarantine the session did not already need.
// ----------------------------------------------------------------
static int terminateError;
static unsigned terminations;
kern_return_t MacLinuxGPU::Terminate(uint64_t options) {
    // Only an instance with nothing of a session left asks for termination.
    assert(!options && session_idle() && !s_stopping);
    events.push_back("terminate");
    ++terminations;
    return terminateError;
}

struct UpgradeRig {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client, observer, next;
    MacLinuxGPUUserClient_IVars clientIvars{}, observerIvars{}, nextIvars{};
    int device = 0;
    uint64_t out[MLG_RETIRE_WORDS]{};
    explicit UpgradeRig(bool session) {
        s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
        // An idle observer (a monitor, the installer): never a participant.
        observer.ivars = &observerIvars;
        observerIvars.ownerDriver = &driver; observerIvars.clientID = 3; observerIvars.observer = true;
        observerIvars.ownerQueue = &s_ownerQueueAtOnce;
        driver.retain();
        if (session) {
            s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
            s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
            bar0Aliases = 1;
            client.ivars = &clientIvars;
            clientIvars.ownerDriver = &driver; clientIvars.clientID = 1;
            clientIvars.ownerQueue = &s_ownerQueueAtOnce;
            clientIvars.sessionGeneration = s_sessionGeneration;
            driver.retain(); s_participants = 1;
        } else {
            // Nothing was ever opened: no interrupt source, DMA or PCI claim.
            irqDrained = dmaCompleted = true;
        }
        next.ivars = &nextIvars;
        nextIvars.ownerDriver = &driver; nextIvars.clientID = 2;
        nextIvars.ownerQueue = &s_ownerQueueAtOnce;
    }
    void deliverIRQDrain() {
        assert(irqCompletion);
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        queue.drain();
    }
    void retire(uint64_t op, bool force = false, uint32_t others = 0) {
        out[0] = out[1] = out[2] = UINT64_MAX;
        retire_driver(&driver, op, force, others, out);
    }
    // IOKit terminates the clients first, then the driver (an upgrade's
    // Retire, a deactivation): Stop only, no other call.
    void kernelStops(bool sessionClient) {
        assert(observer.Stop(&driver) == kIOReturnSuccess);
        assert(!observer.ivars && observer.superStops == 1);
        if (sessionClient) assert(client.Stop(&driver) == kIOReturnSuccess);
        assert(driver.Stop(&provider) == kIOReturnSuccess);
    }
    void assertReleased() const {
        assert(driverStops == 1 && !s_stopProvider && !s_stoppingClients);
        assert(!s_pciOpen && !s_sessionClosing && !s_dmaQuarantined && !s_quarantineRetained);
        assert(driver.references == 1 && provider.references == 1);
    }
};

static const std::vector<std::string> kNormalClose{
    "hold_dma", "compute_stop", "upstream_shutdown", "cancel_irqs",
    "irq_drained", "enqueue_finish", "device_free", "release_bar0",
    "endpoint_reset", "complete_dma", "dma_fini", "pci_close", "gart_reset"};

static std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Stop during an upgrade. "idle": no session, an observer attached: the
// observer's and the driver's Stop finish at once, no queue hop. "session":
// a client's session is open: it closes the normal way and every Stop
// finishes. "quarantined": the close fails to reset the endpoint once (a
// quiescent quarantine): the driver's Stop releases it and finishes.
// "quarantined-held": the compute stop's outcome is uncertain: nothing can
// be released in this process, Stop stays pending with every owner kept and
// the state says restart, never kill.
static void upgradeStop(const std::string &kind) {
    const bool session = kind != "idle";
    if (kind == "quarantined") { resetError = -5; resetFailures = 1; }
    if (kind == "quarantined-held") computeError = -11006;
    UpgradeRig rig(session);
    rig.kernelStops(session);
    if (!session) {
        assert(events == (std::vector<std::string>{"observer_stop", "super_driver_stop"}));
        assert(!irqCompletion && !lxTeardowns && clientStops == 0);
        rig.assertReleased();
        expectLog("stop: no session; provider released at once");
        std::puts("PASS production upgrade stop: idle driver with an observer stops at once");
        return;
    }
    if (kind == "quarantined-held") {
        // The client's release failed: its Stop began the close, which the
        // driver's Stop joined.
        assert(s_stopping && s_stopProvider == &rig.provider && s_stoppingClients == &rig.client);
        assert(!driverStops && !clientStops);
    } else {
        // The client released what it owned and finished: the device stays
        // up across clients. The driver's Stop closes the session.
        assert(s_stopping && s_stopProvider == &rig.provider && !s_stoppingClients);
        assert(!driverStops && clientStops == 1);
    }
    rig.deliverIRQDrain();
    if (kind == "session") {
        assert(events == concat({"observer_stop", "release_client", "super_client_stop"},
                                concat(kNormalClose, {"super_driver_stop"})));
        assert(!saw("pci_quarantine") && clientStops == 1);
        rig.assertReleased();
        expectLog("session closed after upstream removal, interrupt drain and endpoint isolation");
        std::puts("PASS production upgrade stop: open session closes normally, every Stop finishes");
        return;
    }
    if (kind == "quarantined") {
        // The reset failed once while the client stopped; the stopping
        // driver found the quarantine quiescent and released it.
        assert(saw("pci_quarantine") && saw("lift_dma_quarantine") && saw("reopen_pci"));
        assert(events.back() == "super_driver_stop" && saw("super_client_stop") && saw("pci_close"));
        rig.assertReleased();
        expectLog("quarantine released after endpoint reset; provider closed");
        std::puts("PASS production upgrade stop: quiescent quarantine released, every Stop finishes");
        return;
    }
    // Held: owners kept, Stop pending, the readers say restart.
    assert(s_dmaQuarantined && s_sessionClosing && s_quarantineRetained && s_modulesRunning && s_rtDevice);
    assert(!driverStops && !clientStops && s_stopProvider == &rig.provider);
    assert(!saw("pci_close") && !saw("terminate"));
    const auto snapshot = state();
    assert(snapshot[1] & MLG_SESSION_FLAG_RESTART_REQUIRED);
    assert(snapshot[6] == MLG_RELEASE_UPSTREAM_RETAINED);
    expectLog("restart required, do not kill the driver");
    std::puts("PASS production upgrade stop: uncertain session keeps its owners, Stop pending, restart reported");
}

// An upgrade's handover when Retire is refused (the installer is not
// entitled): the installer opens a session client on the previous instance
// and asks ShutdownGPU, which closes the session the device kept up across
// clients; the instance can leave only once its session is closed. "idle":
// the last client already left, the session stayed up; ShutdownGPU closes
// it at once and IOKit's Stop then finds nothing. "clients": a client still
// uses the GPU; ShutdownGPU is refused and remembered, and that client's
// leaving closes the session, with no further call.
static void upgradeHandover(const std::string &kind) {
    UpgradeRig rig(true);
    uint64_t output[2]{};
    IOUserClientMethodArguments arguments{0, output, 2};
    if (kind == "idle") {
        // The only client leaves: what it owned goes, the device stays up.
        assert(rig.client.Stop(&rig.driver) == kIOReturnSuccess);
        assert(s_pciOpen && !s_sessionClosing && s_participants == 0 && clientStops == 1);
        // The installer's session client (never a participant) asks.
        assert(rig.next.shutdown(&arguments) == kIOReturnSuccess);
        assert(output[0] == kIOReturnBusy && output[1] == 2 && s_sessionClosing);
    } else {
        // A client uses the GPU: refused, remembered, said once.
        assert(rig.next.shutdown(&arguments) == kIOReturnBusy);
        assert(s_pciOpen && !s_sessionClosing && s_closeWhenIdle);
        assert(state()[1] & MLG_SESSION_FLAG_CLOSE_WHEN_IDLE);
        expectLog("shutdown: 1 client(s) still use the GPU; the session closes when the last of them leaves");
        assert(rig.next.shutdown(&arguments) == kIOReturnBusy && s_closeWhenIdle);
        // Its leaving closes the session (it releases nothing itself: the
        // close releases everything).
        assert(rig.client.Stop(&rig.driver) == kIOReturnSuccess);
        assert(s_sessionClosing && s_participants == 0 && !saw("release_client"));
    }
    rig.deliverIRQDrain();
    assert(!s_sessionClosing && !s_pciOpen && saw("pci_close") && !saw("pci_quarantine"));
    assert(!s_closeWhenIdle && !(state()[1] & MLG_SESSION_FLAG_CLOSE_WHEN_IDLE));
    assert(rig.next.shutdown(&arguments) == kIOReturnSuccess && output[0] == kIOReturnSuccess && output[1] == 6);
    // IOKit now terminates the instance: its Stop finishes at once.
    assert(rig.observer.Stop(&rig.driver) == kIOReturnSuccess);
    assert(rig.driver.Stop(&rig.provider) == kIOReturnSuccess);
    expectLog("stop: no session; provider released at once");
    rig.assertReleased();
    std::printf("PASS production upgrade handover: %s\n", kind == "idle" ?
                "previous instance with no client closes on ShutdownGPU and stops" :
                "ShutdownGPU with a client waits; its leaving closes the session; the instance stops");
}

static void retireScenario(const std::string &kind) {
    if (kind == "idle") {
        UpgradeRig rig(false);
        rig.retire(MLG_RETIRE_OP_TERMINATE);
        assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_TERMINATING && !rig.out[2]);
        assert(events == std::vector<std::string>{"terminate"} && s_retiring && s_terminateRequested);
        assert(state()[1] & MLG_SESSION_FLAG_RETIRING);
        // Asked again: still terminating, no second request; never resumed.
        rig.retire(MLG_RETIRE_OP_TERMINATE);
        assert(rig.out[1] == MLG_RETIRE_TERMINATING && terminations == 1);
        rig.retire(MLG_RETIRE_OP_RESUME);
        assert(rig.out[0] == kIOReturnNotPermitted && s_retiring);
        // No new session on the retiring instance.
        assert(ensure_open(&rig.next) == kIOReturnNotAttached && !pciOpens);
        rig.kernelStops(false);
        assert(events == (std::vector<std::string>{"terminate", "observer_stop", "super_driver_stop"}));
        rig.assertReleased();
        rig.retire(MLG_RETIRE_OP_TERMINATE);
        assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_STOPPING);
        expectLog("asking IOKit to terminate this driver instance");
        std::puts("PASS production retire: idle instance terminates, refuses sessions, stops at once");
        return;
    }
    if (kind == "quiesce-resume") {
        UpgradeRig rig(true);
        // Another session client is attached: refused unless forced.
        rig.retire(MLG_RETIRE_OP_QUIESCE, false, 1);
        assert(rig.out[0] == kIOReturnBusy && rig.out[1] == MLG_RETIRE_CLIENTS && rig.out[2] == 1);
        assert(events.empty() && !s_retiring && s_pciOpen);
        rig.retire(MLG_RETIRE_OP_QUIESCE, true, 1);
        assert(rig.out[0] == kIOReturnNotReady && rig.out[1] == MLG_RETIRE_CLOSING);
        assert(s_retiring && s_sessionClosing);
        rig.deliverIRQDrain();
        assert(events == kNormalClose && !terminations && !s_dmaQuarantined);
        // The client lost its session; it stays attached, as after any close.
        assert(rig.client.ivars && !s_participants && s_sessionGeneration == 2);
        rig.retire(MLG_RETIRE_OP_QUIESCE);
        assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_IDLE);
        assert(ensure_open(&rig.next) == kIOReturnNotAttached && !pciOpens);
        // The replacement was deferred: the old driver takes sessions again.
        rig.retire(MLG_RETIRE_OP_RESUME);
        assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_RESUMED && !s_retiring);
        assert(!(state()[1] & MLG_SESSION_FLAG_RETIRING));
        pciOpenExpected = true;
        assert(ensure_open(&rig.next) == kIOReturnSuccess && pciOpens == 1 && saw("pci_open"));
        expectLog("retire: closing the session for a driver upgrade (1 other client(s))");
        expectLog("retire: cancelled; new sessions are admitted again");
        std::puts("PASS production retire: quiesce closes the session normally, resume reopens admission");
        return;
    }
    if (kind == "session") {
        UpgradeRig rig(true);
        rig.retire(MLG_RETIRE_OP_TERMINATE, true, 1);
        assert(rig.out[1] == MLG_RETIRE_CLOSING && !terminations);
        rig.deliverIRQDrain();
        // Termination is requested only after the close released everything.
        assert(events == concat(kNormalClose, {"terminate"}));
        // IOKit stops the clients (the session client is no longer a
        // participant) and then the driver, which has nothing left.
        rig.kernelStops(true);
        assert(events == concat(kNormalClose, {"terminate", "observer_stop", "super_client_stop", "super_driver_stop"}));
        assert(clientStops == 1 && !saw("pci_quarantine"));
        rig.assertReleased();
        std::puts("PASS production retire: open session closes, then the instance terminates and stops");
        return;
    }
    if (kind == "disconnect") {
        // Disconnect GPU with programs on the GPU (the device stays up
        // across them): the session closes the normal way for all of them,
        // nothing is refused for clients and new sessions stay admitted.
        UpgradeRig rig(true);
        const uint64_t closedGeneration = s_sessionGeneration;
        assert(rig.clientIvars.sessionGeneration == closedGeneration);
        rig.retire(MLG_RETIRE_OP_DISCONNECT, false, 2);
        assert(rig.out[0] == kIOReturnNotReady && rig.out[1] == MLG_RETIRE_CLOSING && rig.out[2] == 2);
        assert(s_sessionClosing && !s_retiring && s_disconnectedGeneration == closedGeneration);
        rig.retire(MLG_RETIRE_OP_DISCONNECT);
        assert(rig.out[0] == kIOReturnNotReady && rig.out[1] == MLG_RETIRE_CLOSING);
        rig.deliverIRQDrain();
        assert(events == kNormalClose && !terminations && !s_dmaQuarantined && !s_pciOpen);
        // The programs of the closed session keep their clients, marked: the
        // dispatch answers them kIOReturnNoDevice (never a silent rejoin).
        assert(rig.client.ivars && rig.clientIvars.sessionGeneration == s_disconnectedGeneration);
        assert(s_sessionGeneration == closedGeneration + 1 && !s_participants);
        rig.retire(MLG_RETIRE_OP_DISCONNECT);
        assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_IDLE && !s_retiring);
        assert(!(state()[1] & (MLG_SESSION_FLAG_PCI_OPEN | MLG_SESSION_FLAG_RETIRING)));
        // The next program brings the GPU up again.
        pciOpenExpected = true;
        assert(ensure_open(&rig.next) == kIOReturnSuccess && pciOpens == 1 && saw("pci_open"));
        assert(rig.nextIvars.sessionGeneration != s_disconnectedGeneration);
        expectLog("disconnect: closing the session for Disconnect GPU (2 client(s) lose the GPU)");
        std::puts("PASS production disconnect: the session closes for every program, the next one brings the GPU up");
        return;
    }
    if (kind == "disconnect-raw-bar") {
        UpgradeRig rig(true);
        assert(s_rawBARLease.claim(1, true, 1) && s_rawBARLease.markMapped(1));
        rig.retire(MLG_RETIRE_OP_DISCONNECT, false, 1);
        assert(rig.out[0] == kIOReturnBusy && rig.out[1] == MLG_RETIRE_RAW_BAR);
        assert(events.empty() && !s_disconnectedGeneration && !s_dmaQuarantined && s_pciOpen);
        std::puts("PASS production disconnect: a raw BAR mapping refuses the close instead of quarantining");
        return;
    }
    if (kind == "raw-bar") {
        UpgradeRig rig(true);
        assert(s_rawBARLease.claim(1, true, 1) && s_rawBARLease.markMapped(1));
        rig.retire(MLG_RETIRE_OP_TERMINATE, true, 0);
        // A close now would have to quarantine: refused, nothing changes.
        assert(rig.out[0] == kIOReturnBusy && rig.out[1] == MLG_RETIRE_RAW_BAR);
        assert(events.empty() && !s_retiring && !s_dmaQuarantined && s_pciOpen);
        std::puts("PASS production retire: a raw BAR mapping refuses the close instead of quarantining");
        return;
    }
    // A session already quarantined by its close. "quarantined": quiescent
    // (one failed reset), released by Retire, then terminated.
    // "quarantined-held": uncertain compute; Retire reports the permanent
    // blocker and leaves everything (restart, never kill).
    const bool held = kind == "quarantined-held";
    assert(kind == "quarantined" || held);
    if (held) computeError = -11006; else { resetError = -5; resetFailures = 1; }
    UpgradeRig rig(true);
    s_participants = 0; // the client exited; its close quarantined
    close_session(&rig.driver);
    rig.deliverIRQDrain();
    assert(s_dmaQuarantined && saw("pci_quarantine"));
    rig.retire(MLG_RETIRE_OP_TERMINATE);
    if (held) {
        assert(rig.out[0] == kIOReturnError && rig.out[1] == MLG_RETIRE_QUARANTINED);
        assert(rig.out[2] == MLG_RELEASE_UPSTREAM_RETAINED && !terminations && !s_retiring);
        assert(s_dmaQuarantined && s_quarantineRetained && s_modulesRunning);
        expectLog("quarantine release refused");
        std::puts("PASS production retire: an uncertain quarantine is reported, kept and never terminated");
        return;
    }
    assert(rig.out[0] == kIOReturnSuccess && rig.out[1] == MLG_RETIRE_TERMINATING);
    assert(events.back() == "terminate" && saw("lift_dma_quarantine") && saw("pci_close"));
    assert(!s_dmaQuarantined && !s_quarantineRetained && s_retiring);
    rig.kernelStops(true);
    assert(events.back() == "super_driver_stop" && clientStops == 1);
    rig.assertReleased();
    std::puts("PASS production retire: a quiescent quarantine is released, then the instance terminates");
}

// A definite PCI transport fault while programs run (249's MMIO fault),
// then the user power-cycles the GPU: the instance must finish and exit,
// never linger (three 248 instances stayed alive after today's hangs and
// held the upgrade). The fault makes GPU work complete at once, so no wait
// holds the session queue; the program leaves, the close quarantines (its
// DMA hold cannot be taken past the closed admission), and the removal
// releases everything and stops the driver.
static void transportFaultRemoval() {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client;
    MacLinuxGPUUserClient_IVars clientIvars{};
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    client.ivars = &clientIvars;
    clientIvars.ownerDriver = &driver; clientIvars.clientID = 1;
    clientIvars.sessionGeneration = s_sessionGeneration; clientIvars.ownerQueue = &s_ownerQueueAtOnce;
    driver.retain(); s_participants = 1;
    displayShowing = true; surfacesImported = 1;
    // The fault: recorded by the PCI seam, which calls the hook once.
    transportFault = DEXT_PCI_FAULT_MMIO;
    transport_lost(DEXT_PCI_FAULT_MMIO);
    transport_lost(DEXT_PCI_FAULT_MMIO);
    assert(std::count(events.begin(), events.end(), "device_lost") == 1 && s_deviceLost);
    expectLog("the GPU no longer answers this driver");
    // The program leaves (its calls failed): the close cannot hold DMA past
    // the closed admission and quarantines, naming the fault.
    holdError = -5;
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(s_sessionClosing && s_dmaQuarantined && s_quarantineCause == MLG_QUARANTINE_PCI_FAULT);
    {
        assert(irqCompletion);
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        queue.drain();
    }
    assert(s_dmaQuarantined && !clientStops && !driverStops);
    // The power cycle: the device leaves the bus, IOKit stops the provider.
    devicePresent = false;
    driver.retain(); provider.retain();
    assert(driver.Stop(&provider) == kIOReturnSuccess);
    assert(driverStops == 1 && clientStops == 1 && !s_stopProvider && watchdogStarts == 1);
    assert(!s_dmaQuarantined && !s_sessionClosing && !s_deviceRemoved && !s_deviceLost);
    assert(saw("removal_begin") && saw("removal_end") && saw("pci_close_removed"));
    assert(!displayShowing && !surfacesImported);
    expectLog("removal: session released after the device left the bus");
    std::puts("PASS production session shutdown: transport fault, then removal: the instance finishes and stops");
}

// A session call that never returns (a wait for GPU work that cannot
// complete: 250's eviction stall) holds the session queue, where the
// driver's Stop and a Retire run. The watchdog asks once a second; past the
// bound it makes GPU work complete at once, as a removal does, so the call
// returns and the Stop or Retire runs, once.
static void watchdogNeverReturning() {
    const uint64_t s = 1000000000ull;
    assert(!session_watchdog_step(100 * s, "the driver's Stop") && !saw("device_lost"));
    s_ownerJobSince = 5 * s; s_ownerJobSelector = 59;
    assert(!session_watchdog_step(5 * s + (MLG_SESSION_BLOCK_BOUND_MS / 1000 - 1) * s, "Retire"));
    assert(!saw("device_lost") && !s_deviceLost);
    assert(session_watchdog_step(5 * s + (MLG_SESSION_BLOCK_BOUND_MS / 1000 + 1) * s, "Retire"));
    assert(saw("device_lost") && s_deviceLost);
    expectLog("Retire waits behind session call 59, running for 31 s");
    // Asked again (a second watch): nothing more is forced.
    assert(session_watchdog_step(5 * s + 60 * s, "the driver's Stop"));
    assert(std::count(events.begin(), events.end(), "device_lost") == 1);
    // The call returned: the session queue is free again.
    s_ownerJobSince = 0;
    assert(!session_watchdog_step(200 * s, "the driver's Stop"));
    std::puts("PASS production session shutdown: a session call that never returns is ended for Stop and Retire");
}

// Far more programs than the compute backend has records come and go, of
// every kind and by every Stop path: HSA session clients (one leaving, one
// killed with what it owns), Linux-file clients (RADV; one whose QueryInfo
// opened a KFD process), session clients that never joined (one that
// still opened a KFD process), observers (the display agent, mtopg), and
// last clients whose leaving closes the session (legacy, never probed).
// Each Stop lets the client's record go, releasing what it still owns: a
// new program always gets a record, and the session never needs a close.
static void clientChurn() {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    const uint64_t generation = s_sessionGeneration;
    constexpr unsigned rounds = 4 * kClientRecords;
    uint64_t id = 100;
    struct Program {
        MacLinuxGPUUserClient client;
        MacLinuxGPUUserClient_IVars ivars{};
    };
    const auto make = [&](Program &p, bool linuxFile, bool observer) {
        p.ivars = {};
        p.ivars.ownerDriver = &driver;
        p.ivars.clientID = ++id;
        p.ivars.ownerQueue = &s_ownerQueueAtOnce;
        p.ivars.linuxFile = linuxFile;
        p.ivars.observer = observer;
        p.client.ivars = &p.ivars;
        driver.retain();
    };
    const auto lxStop = [&](Program &p) {
        p.ivars.stopping = true;
        p.client.retain(); driver.retain();
        lx_finish_stop(&p.client, &driver);
    };
    // A program stays throughout: no one leaving here is the last.
    Program anchor;
    make(anchor, false, false);
    recordClient(id, true);
    assert(ensure_open(&anchor.client) == kIOReturnSuccess);
    for (unsigned round = 0; round < rounds; ++round) {
        Program hsa, killed, radv, radvKFD, idle, idleKFD, display;
        make(hsa, false, false);
        assert(ensure_open(&hsa.client) == kIOReturnSuccess);
        recordClient(id, true);
        make(killed, false, false);
        assert(ensure_open(&killed.client) == kIOReturnSuccess);
        recordClient(id, true);
        make(radv, true, false);
        assert(ensure_open(&radv.client) == kIOReturnSuccess);
        make(radvKFD, true, false);
        assert(ensure_open(&radvKFD.client) == kIOReturnSuccess);
        recordClient(id, true);
        make(idle, false, false);
        recordClient(id, false);
        make(idleKFD, false, false);
        recordClient(id, true);
        make(display, false, true);
        assert(s_participants == 5 && records.size() == 6);
        // A killed process's client stops as any other does.
        assert(killed.client.Stop(&driver) == kIOReturnSuccess);
        assert(hsa.client.Stop(&driver) == kIOReturnSuccess);
        lxStop(radv);
        lxStop(radvKFD);
        assert(idle.client.Stop(&driver) == kIOReturnSuccess);
        assert(idleKFD.client.Stop(&driver) == kIOReturnSuccess);
        assert(display.client.Stop(&driver) == kIOReturnSuccess);
        assert(s_participants == 1 && !s_sessionClosing && s_sessionGeneration == generation);
        assert(records == std::vector<uint64_t>({anchor.ivars.clientID}));
        assert(owners == std::vector<uint64_t>({anchor.ivars.clientID}));
    }
    assert(!saw("upstream_shutdown") && !s_dmaQuarantined && pciOpens == 0);
    assert(clientStops == 6 * rounds);
    // A release that fails keeps that client's record, counted, and
    // taints the session (the anchor's KFD process could not be closed).
    releaseError = -16;
    {
        Program last;
        make(last, false, false);
        assert(ensure_open(&last.client) == kIOReturnSuccess);
        recordClient(id, true);
        assert(last.client.Stop(&driver) == kIOReturnSuccess);
        assert(s_dmaQuarantined && records.size() == 2 && has(records, id));
        expectLog("compute record kept, its KFD process not closed (2 record(s) held)");
    }
    std::puts("PASS production session shutdown: client-churn");
}

// The last client's leaving closes the session (a legacy client, then a
// session that was never probed), many times over: each close lets that
// client's record go, so the next session's client gets one.
static void lastLeaverChurn() {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue; s_stopQueue = &s_ownerQueueAtOnce;
    constexpr unsigned rounds = 4 * kClientRecords;
    for (unsigned round = 0; round < rounds; ++round) {
        const bool probed = round % 2 == 0;
        irqDrained = endpointReset = dmaCompleted = false;
        events.clear();
        s_rtDevice = probed ? &device : nullptr; s_modulesRunning = probed; s_probeAttempted = probed;
        s_irqReady = s_irqDeliver = true; s_pciOpen = true; s_token = 7;
        bar0Aliases = probed ? 1 : 0;
        MacLinuxGPUUserClient client;
        MacLinuxGPUUserClient_IVars ivars{};
        ivars.ownerDriver = &driver; ivars.clientID = 5000 + round;
        ivars.ownerQueue = &s_ownerQueueAtOnce; ivars.sessionGeneration = s_sessionGeneration;
        client.ivars = &ivars;
        driver.retain(); s_participants = 1;
        legacyClientID = probed ? ivars.clientID : 0;
        recordClient(ivars.clientID, probed);
        assert(client.Stop(&driver) == kIOReturnSuccess);
        assert(s_sessionClosing && s_participants == 0 && !saw("release_client"));
        assert(probed == saw("upstream_shutdown"));
        // The close released what the client owned (dext_compute_stop);
        // its record went with its Stop.
        erase(owners, ivars.clientID);
        assert(records.empty());
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        queue.drain();
        assert(!s_sessionClosing && !s_pciOpen && clientStops == round + 1 && saw("pci_close"));
    }
    assert(driver.references == 1);
    std::puts("PASS production session shutdown: last-leaver-churn");
}

static rt_drm_info observerDrm;
int main(int argc, char **argv) {
    alarm(15);
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "concurrent-clients") {
        concurrentClients();
        return 0;
    }
    if (scenario == "watchdog-never-returning") {
        watchdogNeverReturning();
        return 0;
    }
    if (scenario == "transport-fault-removal") {
        transportFaultRemoval();
        return 0;
    }
    if (scenario == "client-churn") {
        clientChurn();
        return 0;
    }
    if (scenario == "last-leaver-churn") {
        lastLeaverChurn();
        return 0;
    }
    if (scenario == "client-exit-unprobed" || scenario == "client-exit-release-failure" ||
        scenario == "client-exit-raw-mapped" || scenario == "client-exit-legacy") {
        clientExitCloses(scenario);
        return 0;
    }
    if (scenario == "client-exit-reopen" || scenario == "queue-exhaustion-exit") {
        clientExitReopen(scenario == "queue-exhaustion-exit");
        return 0;
    }
    if (scenario == "surprise-removal" || scenario == "surprise-removal-quarantined" ||
        scenario == "surprise-removal-held") {
        surpriseRemoval(scenario != "surprise-removal", scenario == "surprise-removal-held");
        return 0;
    }
    if (scenario.rfind("upgrade-stop-", 0) == 0) {
        upgradeStop(scenario.substr(13));
        return 0;
    }
    if (scenario.rfind("upgrade-handover-", 0) == 0) {
        upgradeHandover(scenario.substr(17));
        return 0;
    }
    if (scenario.rfind("retire-", 0) == 0) {
        retireScenario(scenario.substr(7));
        return 0;
    }
    Fixture fixture;
    if (scenario == "log-format") {
        checkLogFormat();
        checkObserverPolicy();
        std::puts("PASS lifecycle log: single printf evaluation, bounded text, retained/platform equality and sink reentry; observer selector policy");
        return 0;
    }
    if (scenario == "shutdown-selector") {
        checkSelectorGuards(fixture);
        std::puts("PASS production shutdown selector: peer/raw-BAR rejection, busy and completed phases");
        return 0;
    }
    if (scenario == "hold-failure") holdError = 1;
    else if (scenario == "compute-failure") computeError = 1;
    else if (scenario == "irq-failure" || scenario == "irq-failure-late") irqError = 1;
    else if (scenario == "reset-failure") resetError = 1;
    else if (scenario == "dma-fini-failure") dmaFiniError = 1;
    else if (scenario == "pre-quarantined") s_dmaQuarantined = true;
    else if (scenario == "isolation-failure") { s_dmaQuarantined = true; isolationError = 1; }
    else if (scenario == "raw-mapped") {
        assert(s_rawBARLease.claim(1, true, 1) && s_rawBARLease.markMapped(1));
    }
    else if (scenario == "probe-retained") {
        probeCleanupRetained = true;
        s_probeResult = -38;
        s_probeAttempted = true;
    }
    else if (scenario == "observer-quarantined" || scenario == "release-after-isolation-failure") {
        resetError = -5;
    }
    else if (scenario == "release-refused-upstream") computeError = -11006;
    else if (scenario == "release-reset-failed") resetError = -110;
    else if (scenario == "stop-release") { resetError = -5; resetFailures = 1; s_stopping = true; }
    else if (scenario == "pci-fault-cause") { transportFault = DEXT_PCI_FAULT_CONFIG; holdError = -1; }
    else if (scenario == "selftest-parked") lxParked = 1;
    else if (scenario == "display-showing") { displayShowing = true; surfacesImported = 2; }
    else if (scenario == "display-quarantined") {
        displayShowing = true; surfacesImported = 1; s_dmaQuarantined = true;
    }
    else if (scenario == "observer-reads") {
        // A running session admits observer reads; one is in flight and the
        // observers' render file is open.
        s_observerReads.open();
        assert(s_observerReads.enter());
        observerReadInFlight = true;
        s_observerDrm = &observerDrm;
    }
    else if (scenario == "large-mapped") mappedBytes = 6ull << 30;	// past any old budget
    else assert(scenario == "success");
    if (scenario == "probe-retained")
        assert(fixture.observer.failedProbe() == kIOReturnError);
    else close_session(&fixture.driver);
    assert(!clientStops && !driverStops && s_sessionClosing && s_rtDevice);
    assert(!s_irqDeliver && !s_irqReady && !irqDrained);
    const auto eventCount = events.size();
    const auto references = fixture.driver.references;
    close_session(&fixture.driver);
    assert(events.size() == eventCount && fixture.driver.references == references);
    if (scenario != "irq-failure") {
        // Before the drain completes nothing can be released.
        if (s_dmaQuarantined && !s_irqDrainFailed && transportFault == DEXT_PCI_FAULT_QUARANTINE)
            assert(state()[6] == MLG_RELEASE_IRQ_PENDING);
        fixture.deliverIRQDrain();
    }
    if (scenario == "observer-reads") {
        // Admission closes and drains before anything is torn down, and the
        // render file closes before the upstream driver is removed.
        const std::vector<std::string> expected{
            "observer_wait", "hold_dma", "compute_stop", "observer_drm_close",
            "upstream_shutdown", "cancel_irqs",
            "irq_drained", "enqueue_finish", "device_free", "release_bar0",
            "endpoint_reset", "complete_dma", "dma_fini", "pci_close", "gart_reset",
            "super_client_stop", "super_client_stop", "super_driver_stop"};
        assert(events == expected);
        assert(observerDrm.closes == 1 && !s_observerDrm && !s_observerReads.enter());
        fixture.assertReleased();
    } else if (scenario == "display-showing") {
        // The pattern goes first, before Linux-file teardown and removal.
        const std::vector<std::string> expected{
            "display_off", "surfaces_release", "hold_dma", "compute_stop", "upstream_shutdown", "cancel_irqs",
            "irq_drained", "enqueue_finish", "device_free", "release_bar0",
            "endpoint_reset", "complete_dma", "dma_fini", "pci_close", "gart_reset",
            "super_client_stop", "super_client_stop", "super_driver_stop"};
        assert(events == expected && !displayShowing);
        fixture.assertReleased();
        expectLog("session close: turning the display pattern or output off");
        expectLog("session close: released 2 imported surface(s)");
    } else if (scenario == "success" || scenario == "large-mapped") {
        assert(heldBytes == mappedBytes);
        const std::vector<std::string> expected{
            "hold_dma", "compute_stop", "upstream_shutdown", "cancel_irqs",
            "irq_drained", "enqueue_finish", "device_free", "release_bar0",
            "endpoint_reset", "complete_dma", "dma_fini", "pci_close", "gart_reset",
            "super_client_stop", "super_client_stop", "super_driver_stop"};
        assert(events == expected);
        assert(lxTeardowns == 1);
        fixture.assertReleased();
        expectLog("session closed after upstream removal, interrupt drain and endpoint isolation");
    } else if (scenario == "stop-release") {
        // Deactivation must not stall on a quiescent quarantine: the stopping
        // driver retries the reset at once and finishes every Stop.
        fixture.assertReleased();
        assert(saw("lift_dma_quarantine") && saw("reopen_pci") && saw("pci_close"));
        expectLog("quarantine cause: step 5 code -5");
        expectLog("quarantine released after endpoint reset; provider closed");
    } else {
        fixture.assertRetained(scenario == "irq-failure");
        assert(!saw("pci_close") && !saw("gart_reset"));
        if (holdError || computeError || scenario == "pre-quarantined" || scenario == "display-quarantined" ||
            scenario == "raw-mapped" || scenario == "isolation-failure" ||
            scenario == "probe-retained" || lxParked) {
            assert(s_rtDevice && s_modulesRunning);
            assert(!saw("device_free") && !saw("endpoint_reset") && !saw("dma_fini"));
        }
        if (lxParked) {
            // The self-test's work is still on the GPU: nothing upstream is
            // stopped or removed under it.
            assert(events.front() == "lx_teardown_parked");
            assert(!saw("hold_dma") && !saw("compute_stop") && !saw("upstream_shutdown"));
        }
        if (scenario == "raw-mapped") {
            assert(!saw("hold_dma") && !saw("compute_stop") && !saw("upstream_shutdown"));
            assert(s_rawBARLease.hasMappings() && !s_rawBARLease.allowsJoin(2));
        }
        if (scenario == "probe-retained") {
            assert(!saw("hold_dma") && !saw("compute_stop") && !saw("upstream_shutdown"));
            assert(s_modulesRunning && s_rtDevice == &fixture.device);
            assert(s_probeAttempted && s_probeResult == -38);
            expectLog("upstream PCI probe failed: -38");
            expectLog("upstream failed-probe ownership retained; preserving modules, device and DMA backing");
        }
        if (resetError) assert(!saw("complete_dma") && !saw("dma_fini"));
        // Repeated quarantine must not acquire an unbounded service retain.
        const auto retained = fixture.driver.references;
        quarantine_session(&fixture.driver);
        assert(fixture.driver.references == retained);
        uint64_t output[2]{};
        IOUserClientMethodArguments arguments{0, output, 2};
        if (scenario == "raw-mapped")
            assert(fixture.observer.shutdown(&arguments) == kIOReturnBusy);
        else {
            assert(fixture.observer.shutdown(&arguments) == kIOReturnSuccess);
            assert(output[0] == kIOReturnError && output[1] == 5);
        }
        if (holdError == 1) expectLog("cannot reserve DMA backing for shutdown (1)");
        if (computeError == 1) expectLog("GPU completion uncertain (1)");
        if (irqError) expectLog("interrupt cancellation failed (1)");
        if (resetError == 1) expectLog("endpoint isolation failed (1)");
        if (dmaFiniError) expectLog("live DMA backing retained (1)");
        if (isolationError) expectLog("PCI isolation unconfirmed (1)");
        if (scenario == "raw-mapped") expectLog("raw BAR mapping lifetime uncertain");
        if (scenario != "irq-failure")
            expectLog("session quarantined: retaining clients, provider and runtime owners");

        // Every quarantine names its step through the cached session state.
        const auto snapshot = state();
        assert(snapshot[1] & MLG_SESSION_FLAG_QUARANTINED);
        assert(snapshot[1] & MLG_SESSION_FLAG_CLOSING);
        uint32_t cause = MLG_QUARANTINE_NONE;
        if (scenario == "hold-failure") cause = MLG_QUARANTINE_SHUTDOWN_HOLD;
        else if (computeError) cause = MLG_QUARANTINE_COMPUTE_UNCERTAIN;
        else if (irqError) cause = MLG_QUARANTINE_IRQ_CANCEL;
        else if (resetError) cause = MLG_QUARANTINE_ENDPOINT_ISOLATION;
        else if (dmaFiniError) cause = MLG_QUARANTINE_DMA_RETAINED;
        else if (scenario == "raw-mapped") cause = MLG_QUARANTINE_RAW_BAR_MAPPING;
        else if (scenario == "probe-retained") cause = MLG_QUARANTINE_PROBE_RETAINED;
        else if (scenario == "pci-fault-cause") cause = MLG_QUARANTINE_PCI_FAULT;
        else if (lxParked) cause = MLG_QUARANTINE_COMPUTE_UNCERTAIN;
        assert(snapshot[2] == cause);
        if (lxParked) assert(snapshot[6] == MLG_RELEASE_UPSTREAM_RETAINED);
        if (scenario == "pci-fault-cause") {
            assert(snapshot[3] == DEXT_PCI_FAULT_CONFIG);
            assert(snapshot[4] == MLG_QUARANTINE_SHUTDOWN_HOLD);
            assert(snapshot[6] == MLG_RELEASE_PCI_FAULT);
        }
        if (resetError || dmaFiniError) assert(snapshot[3] == (uint64_t)(int64_t)(resetError ? resetError : dmaFiniError));
        if (computeError || holdError || scenario == "pre-quarantined" || scenario == "display-quarantined" ||
            scenario == "isolation-failure" || scenario == "raw-mapped" ||
            scenario == "probe-retained" || irqError || lxParked) {
            // Owners that were never removed (or live callbacks) cannot be
            // released in this process: the readers say restart, never kill.
            assert(snapshot[1] & MLG_SESSION_FLAG_RESTART_REQUIRED);
            assert(mlg_release_blocker_permanent((uint32_t)snapshot[6]));
            const auto before = events.size();
            assert(release_quarantine(&fixture.driver) == snapshot[6]);
            assert(events.size() == before);
            fixture.assertRetained(scenario == "irq-failure");
            expectLog("quarantine release refused");
            if (scenario == "release-refused-upstream") {
                assert(snapshot[6] == MLG_RELEASE_UPSTREAM_RETAINED);
                expectLog("restart required, do not kill the driver");
            }
        }
        if (scenario == "observer-quarantined") {
            // An observer is never a participant: stopping it while
            // quarantined finishes at once and changes no session state.
            const auto before = events.size();
            const auto participants = s_participants;
            fixture.driver.retain(); // the observer's Start retained its owner
            fixture.observer.ivars->sessionGeneration = 0;
            fixture.observer.ivars->observer = true;
            assert(ensure_open(&fixture.observer) == kIOReturnNotPermitted);
            assert(fixture.observer.Stop(&fixture.driver) == kIOReturnSuccess);
            assert(events.size() == before + 1 && events.back() == "observer_stop");
            assert(s_participants == participants && s_stoppingClients == &fixture.clients[0]);
            assert(!fixture.observer.ivars && fixture.observer.superStops == 1);
            fixture.assertRetained(false);
            assert(state()[6] == MLG_RELEASE_READY);
        }
        if (scenario == "release-after-isolation-failure" || scenario == "dma-fini-failure" ||
            scenario == "reset-failure") {
            assert(snapshot[1] & MLG_SESSION_FLAG_RELEASABLE);
            assert(!(snapshot[1] & MLG_SESSION_FLAG_RESTART_REQUIRED));
            resetError = 0;
            assert(release_quarantine(&fixture.driver) == MLG_RELEASE_READY);
            fixture.assertReleased();
            expectLog("quarantine released after endpoint reset; provider closed");
        }
        if (scenario == "release-reset-failed") {
            assert(snapshot[6] == MLG_RELEASE_READY);
            assert(release_quarantine(&fixture.driver) == MLG_RELEASE_RESET_FAILED);
            fixture.assertRetained(false);
            const auto after = state();
            assert(after[6] == MLG_RELEASE_RESET_FAILED);
            assert(after[1] & MLG_SESSION_FLAG_RESTART_REQUIRED);
            assert(after[2] == MLG_QUARANTINE_ENDPOINT_ISOLATION);
            expectLog("quarantine release failed (-110); restart required, do not kill the driver");
        }
    }
    if (scenario == "probe-retained")
        expectLog("session close begin: probe=1 result=-38 modules=1 pci=1 quarantine=1");
    else expectLog("session close begin: probe=0 result=0 modules=1 pci=1 quarantine=");
    expectLog("session close: requesting interrupt drain");
    if (scenario != "irq-failure") {
        expectLog("interrupt drain completed; scheduling final cleanup");
        expectLog("session close: final cleanup entered");
    }
    std::printf("PASS production session shutdown: %s\n", argv[1]);
}
