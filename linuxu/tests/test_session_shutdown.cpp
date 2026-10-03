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
enum { DEXT_PCI_FAULT_NONE = 0, DEXT_PCI_FAULT_CONFIG = 1, DEXT_PCI_FAULT_QUARANTINE = 4 };
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
    void DispatchAsync(Block block) {
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
struct MacLinuxGPU : IOService {
    void FinishSession();
    void FinishStop(IOService *provider);
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

static uint64_t linuxu_dart_budget();
static int dext_dma_begin_shutdown(uint64_t);
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

#include "session_shutdown_production.inc"

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

static void *rt_device_get_pdev(void *device) {
    assert(device && device == s_rtDevice);
    return device;
}
static int rt_pci_probe_cleanup_retained(void *device) {
    assert(device && device == s_rtDevice);
    return probeCleanupRetained;
}

static uint64_t linuxu_dart_budget() { return 1ull << 30; }
static int dext_dma_begin_shutdown(uint64_t budget) {
    assert(budget == linuxu_dart_budget() && s_irqDeliver && s_irqReady);
    events.push_back("hold_dma");
    return holdError;
}
static int dext_compute_stop() {
    assert(s_irqDeliver && s_irqReady && s_dmaShutdownPrepared);
    events.push_back("compute_stop");
    if (!computeError) computeQuiescent = true;
    else computeQuiescent = false;
    return computeError;
}
static void linuxu_driver_shutdown() {
    assert(s_irqDeliver && s_irqReady && s_modulesRunning && s_dmaShutdownPrepared);
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
    assert(!s_modulesRunning && !s_irqDeliver && s_dmaShutdownPrepared);
    assert(!endpointReset && !dmaCompleted);
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
    assert(irqDrained && endpointReset && dmaCompleted && !s_rtDevice);
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
static int dext_compute_release_client(uint64_t) { assert(false); return 0; }

void MacLinuxGPU::FinishStop(IOService *provider) {
    assert(!s_dmaQuarantined && !s_sessionClosing && dmaCompleted);
    events.push_back("super_driver_stop"); ++driverStops;
    provider->release(); release();
}
void MacLinuxGPUUserClient::FinishStop(IOService *provider) {
    if (!ivars->observer)
        assert(!s_dmaQuarantined && !s_sessionClosing && dmaCompleted);
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
        s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue;
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
            ivars[i].nextStopping = i ? nullptr : &clients[1];
            driver.retain(); driver.retain(); clients[i].retain();
        }
        s_stoppingClients = &clients[0];
        observer.ivars = &ivars[2];
        ivars[2].ownerDriver = &driver; ivars[2].clientID = 3;
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
    const uint64_t read[] = {MLG_SYSFS_OP_READ, 0}, list[] = {MLG_SYSFS_OP_LIST, 4096};
    const uint64_t write[] = {2, 0}, info[] = {0x1d, 4}, huge[] = {0x1d, MLG_SYSFS_CHUNK_MAX + 1};
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, read, 2));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, list, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, write, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_SYSFS_READ, read, 1));
    assert(mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, info, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, huge, 2));
    assert(!mlg_observer_selector_allowed(MLG_SELECTOR_DRM_INFO, info, 1));
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
            selector == MLG_SELECTOR_RELEASE_QUARANTINE) continue;
        assert(!mlg_observer_selector_allowed(selector, probe, 1));
    }
}

// Successful probe and compute start, then the only client exits without
// destroying anything: no quarantine, and a new client opens a new session.
static void clientExitReopen(bool queueExhausted) {
    MacLinuxGPU driver;
    IOPCIDevice provider;
    IODispatchQueue queue;
    MacLinuxGPUUserClient client, next;
    MacLinuxGPUUserClient_IVars clientIvars{}, nextIvars{};
    int device = 0;
    s_driver = &driver; s_retainedPCI = &provider; s_bringupQueue = &queue;
    s_rtDevice = &device; s_modulesRunning = true; s_probeAttempted = true;
    s_irqReady = s_irqDeliver = s_pciOpen = true; s_token = 7;
    bar0Aliases = 1;
    client.ivars = &clientIvars;
    clientIvars = {&driver, nullptr, nullptr, s_sessionGeneration, 1, false, false, false, nullptr, false,
                   false, nullptr, nullptr};
    driver.retain(); s_participants = 1;
    // A second queue found every slot held. The runtime refused it before
    // any allocation (or the driver did, with -ENOSPC before reserving), so
    // compute stop sees only the first queue and its BOs.
    if (queueExhausted) computeError = 0;
    assert(client.Stop(&driver) == kIOReturnSuccess);
    assert(s_sessionClosing && s_participants == 0 && !clientStops);
    {
        assert(irqCompletion);
        auto callback = irqCompletion; auto context = irqContext;
        irqCompletion = nullptr; irqContext = nullptr;
        irqDrained = true; events.push_back("irq_drained");
        callback(context);
        queue.drain();
    }
    const std::vector<std::string> expected{
        "hold_dma", "compute_stop", "upstream_shutdown", "cancel_irqs",
        "irq_drained", "enqueue_finish", "device_free", "release_bar0",
        "endpoint_reset", "complete_dma", "dma_fini", "pci_close", "gart_reset",
        "super_client_stop"};
    assert(events == expected);
    assert(!s_dmaQuarantined && !s_sessionClosing && !s_quarantineRetained);
    assert(s_sessionGeneration == 2 && clientStops == 1 && client.superStops == 1);
    assert(driver.references == 1 && provider.references == 1);
    const auto snapshot = state();
    assert(!(snapshot[1] & (MLG_SESSION_FLAG_QUARANTINED | MLG_SESSION_FLAG_CLOSING)));
    assert(snapshot[2] == MLG_QUARANTINE_NONE && snapshot[6] == MLG_RELEASE_NOT_QUARANTINED);
    expectLog("released 1 orphaned BAR0 CPU mapping reference(s) after upstream removal");
    expectLog("session closed after upstream removal, interrupt drain and endpoint isolation");
    // The dext is reusable: a new client opens a new PCI session.
    next.ivars = &nextIvars;
    nextIvars = {&driver, nullptr, nullptr, 0, 2, false, false, false, nullptr, false,
                 false, nullptr, nullptr};
    pciOpenExpected = true;
    assert(ensure_open(&next) == kIOReturnSuccess);
    assert(s_pciOpen && s_participants == 1 && pciOpens == 1 && saw("pci_open"));
    assert(nextIvars.sessionGeneration == s_sessionGeneration);
    std::printf("PASS production session shutdown: %s\n",
                queueExhausted ? "queue-exhaustion-exit" : "client-exit-reopen");
}

static rt_drm_info observerDrm;
int main(int argc, char **argv) {
    alarm(15);
    assert(argc == 2);
    const std::string scenario = argv[1];
    if (scenario == "client-exit-reopen" || scenario == "queue-exhaustion-exit") {
        clientExitReopen(scenario == "queue-exhaustion-exit");
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
    else if (scenario == "observer-reads") {
        // A running session admits observer reads; one is in flight and the
        // observers' render file is open.
        s_observerReads.open();
        assert(s_observerReads.enter());
        observerReadInFlight = true;
        s_observerDrm = &observerDrm;
    }
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
    } else if (scenario == "success") {
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
        if (holdError || computeError || scenario == "pre-quarantined" ||
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
        if (computeError || holdError || scenario == "pre-quarantined" ||
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
