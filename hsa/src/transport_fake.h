#pragma once

#include "transport.h"
#include "device_init.h"
#include <map>
#include <mutex>
#include <condition_variable>
#include <string>
#include <array>
#include <span>
#include <vector>

// Default-visibility decorator for the fake's public API. The dylib is built
// with hidden visibility (only HSA_API_EXPORT symbols are exported); the fake
// must be reachable from the host tests (which link the dylib), so its class +
// accessors are marked default-visible.
#define MAC_HSA_FAKE_EXPORT __attribute__((visibility("default")))

// In-memory Connection backend for host unit-testing. No IOKit, no dext, no
// GPU. Models the device snapshot, buffer registry, shared buffers, queues,
// and AQL/compute dispatch with in-process state so the runtime's packet
// construction, signal state machine, queue management, and memory allocation
// are exercisable on the host.
//
// Not the real transport: the IOKit transport (transport_iokit.cpp) is the one
// that talks to the dext over the user-client selector-RPC. This fake stands in
// for it so the runtime's logic (which is transport-agnostic, calling only
// Connection virtual methods) can be unit-tested without hardware.

namespace mac_hsa {

// The device the fake reports, field for field what the driver's QueryInfo
// tags carry. Tests configure non-default devices with setFakeDeviceConfig.
struct FakeDeviceConfig {
    uint64_t build = kQueueResourceDriverBuild;
    uint32_t gcMajor = 11, gcMinor = 0, gcRevision = 0;   // tag 1
    uint64_t sdmaIP = 0x060000, mmhubIP = 0x030000, mp0IP = 0x0d0000; // tag 3, packed
    uint64_t visibleVRAM = 16ull << 30, totalVRAM = 16ull << 30; // tag 2
    // tag 6
    uint32_t chipID = 0x0abc, revision = 0x01, bdf = 0x0300, domain = 0;
    uint32_t computeUnits = 48, shaderEngines = 6, arraysPerEngine = 2;
    uint64_t timestampFrequency = 100000000;
    uint32_t wavefrontSize = 32;
    // Topology tag; topologyReported=false models a driver that predates it.
    bool topologyReported = true;
    uint32_t gfxTargetVersion = 0; // 0: report KFD's mapping of the GC version
    uint32_t simdPerCU = 2, maxWavesPerSIMD = 16;
    uint32_t groupSegmentBytes = 65536, privateSegmentBytes = 131072, scratchSlotsPerCU = 32;
    uint32_t queueSlots = 4, queueMinPackets = 64, queueMaxPackets = 4096;
    // Compute session the driver reports (tag 12). On the KFD path queueSlots
    // is KFD's per-process limit rather than a count of legacy HQDs.
    ComputeSessionMode sessionMode = ComputeSessionMode::Legacy;
    uint32_t l1CacheBytes = 32768, l2CacheBytes = 4u << 20, l3CacheBytes = 64u << 20;
    uint32_t maxClockMHz = 2400, xccCount = 1;
    TargetFeature sramecc = TargetFeature::Unsupported;
    std::string productName = "Fake AMDGPU";            // product-name tag; empty = declined
    // Interrupt signals (selectors 86/87): a KFD session on a driver that
    // serves them.
    bool signalEvents = false;
};

class MAC_HSA_FAKE_EXPORT FakeConnection final : public Connection, public InitializationRPC {
public:
    FakeConnection();
    explicit FakeConnection(const FakeDeviceConfig &config);

    // ---- Connection: device snapshot ----
    hsa_status_t read(DeviceSnapshot &snapshot) override;
    bool supportsBuffers() const override { return true; }
    bool supportsSharedBuffers() const override { return true; }
    hsa_status_t properties(DeviceProperties &out) override;

    // ---- Connection: buffers (VRAM) ----
    hsa_status_t allocateBuffer(uint64_t size, DeviceBuffer &out) override;
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override;
    hsa_status_t readBuffer(const DeviceBuffer &buffer, uint64_t offset, void *dst, size_t size) override;
    hsa_status_t writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *src, size_t size) override;
    hsa_status_t copyBuffers(const DeviceBuffer &dst, uint64_t dstOffset,
                             const DeviceBuffer &src, uint64_t srcOffset, size_t size) override;
    hsa_status_t invalidateCodeCaches() override;
    hsa_status_t exportBuffer(const DeviceBuffer &buffer, BufferToken &token) override;
    hsa_status_t importBuffer(const BufferToken &token, DeviceBuffer &out) override;

    // ---- Connection: shared (GTT) buffers ----
    hsa_status_t sharedMemoryCapacity(uint64_t &capacity) override;
    hsa_status_t memoryCapacity(uint64_t &capacity) override;
    hsa_status_t memoryAvailable(uint64_t &available) override;
    hsa_status_t allocateSharedBuffer(uint64_t size, SharedBuffer &out) override;
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override;
    hsa_status_t testSharedAtomicAdd(const SharedBuffer &buffer, uint64_t offset,
                                     int64_t value, uint32_t result) override;

    // ---- Connection: queues ----
    bool queueSlotsExhausted() override;
    hsa_status_t createQueue(const SharedBuffer &ring, const SharedBuffer &metadata,
                             uint32_t size, uint64_t &handle) override;
    hsa_status_t kickQueue(uint64_t handle, uint64_t doorbell) override;
    hsa_status_t destroyQueue(uint64_t handle) override;
    hsa_status_t serviceQueue(uint64_t handle, uint64_t &inactive) override;
    bool memoryFault(MemoryFault &out) override;

    // ---- Connection: dispatch ----
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &request, uint64_t &fence) override;
    hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &request, uint64_t &fence) override;

    // ---- Connection: device power (power.h) ----
    hsa_status_t powerState(PowerSnapshot &out) override;
    hsa_status_t requestPower(uint64_t op, PowerSnapshot &out) override;

    // ---- Connection: interrupt signals (KFD signal events) ----
    // Served when config().signalEvents (a KFD session on a driver with
    // events); declined otherwise, as the real driver declines.
    hsa_status_t createSignalEvent(SignalEvent &out, std::string *why) override;
    hsa_status_t destroySignalEvent(uint32_t id) override;
    hsa_status_t setSignalEvent(uint32_t id) override;
    hsa_status_t waitSignalEvents(const uint32_t *ids, uint32_t count, uint32_t timeoutMs,
                                  EventWaitResult &result) override;
    // The command processor completing a packet that names @signal:
    // decrement its value and, for an interrupt signal, write its mailbox
    // and raise the interrupt, unless interrupts are being dropped.
    void completeSignal(uint64_t signalHandle);
    void dropInterrupts(bool drop);
    struct EventStats {
        uint64_t created = 0, live = 0, interrupts = 0, dropped = 0, sets = 0;
        uint64_t waits = 0, firedWaits = 0, timedOutWaits = 0;
    };
    EventStats eventStats() const;

    // ---- InitializationRPC (used by device_init.cpp) ----
    hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input,
                        std::span<uint64_t> output) override;
    hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) override;
    hsa_status_t uploadFirmware(const FirmwareFile &file) override;
    void waitAfterReset() override;

    // Test observability: the last AQL/compute dispatch request (for packet-layout tests).
    const amdgpu::AQLDispatchRequest *lastAQL() const;
    const amdgpu::ComputeDispatchRequest *lastCompute() const;
    uint64_t kickCount(uint64_t handle) const;
    size_t queueCount() const;
    size_t bufferCount() const;
    uint64_t aqlDispatchCount() const;
    // Code-cache synchronizations: through the driver's bounded launch
    // (invalidateCodeCaches), and as AQL packets the fake packet processor
    // executed on a runtime queue.
    uint64_t codeSyncCount() const;
    uint64_t queueCodeSyncCount() const;
    // Doorbells rung on every queue when the last code sync ran.
    uint64_t kicksAtLastCodeSync() const;
    // Device power, as the driver reports it. While the state is
    // suspending, suspended or resuming, GPU work (kicks, queue service and
    // creation, allocation, launches) is refused with
    // kDeviceSuspendedStatus and nothing happens; while lost, the closed
    // session's calls fail. PREPARE from active suspends with memory kept;
    // the last RESUME makes it active again. Every change bumps the
    // generation.
    void setPowerState(amdgpu::power::PowerState state, bool vramPreserved = true);
    // The process's GPU work faults at @address (KFD evicts its queues, as
    // the driver reports from kQueueFaultDriverBuild on): queue service and
    // kicks answer kMemoryFaultStatus from now on, and nothing runs.
    void injectMemoryFault(uint64_t address, uint32_t reason);
    uint64_t powerRequests(uint64_t op) const;
    const FakeDeviceConfig &config() const { return config_; }

private:
    struct Buffer {
        void *host = nullptr;
        size_t size = 0;
        bool shared = false; // true = shared GTT (host-mapped), false = VRAM
    };
    struct Queue {
        uint32_t size = 0;
        uint64_t doorbell = 0;
        uint64_t kicks = 0;
        bool active = true;
        void *ring = nullptr, *metadata = nullptr;
    };

    mutable std::mutex mutex_;
    std::map<uint64_t, Buffer> buffers_;
    std::map<uint64_t, Queue> queues_;
    uint64_t nextBufferHandle_ = 1;
    uint64_t nextQueueHandle_ = 1;
    amdgpu::AQLDispatchRequest lastAQL_{};
    bool hasLastAQL_ = false;
    amdgpu::ComputeDispatchRequest lastCompute_{};
    bool hasLastCompute_ = false;
    std::vector<FirmwareFile> preparedFirmware_;
    std::vector<std::string> uploadedFirmware_;
    uint64_t aqlDispatches_ = 0;
    uint64_t codeSyncs_ = 0, queueCodeSyncs_ = 0, kicksAtCodeSync_ = 0;
    FakeDeviceConfig config_;
    DeviceSnapshot snapshot_;
    std::array<uint64_t, kTopologyQueryWords> topology_{};
    std::array<uint64_t, kProductNameQueryWords> productName_{};
    std::array<uint64_t, 10> tag6_{};
    bool queueSlotsExhaustedLocked() const;
    void processQueueLocked(Queue &queue);
    PowerSnapshot power_;
    bool faulted_ = false;
    MemoryFault fault_;
    uint64_t powerRequests_[4]{};
    // kDeviceSuspendedStatus or HSA_STATUS_ERROR when the power state
    // refuses GPU work, HSA_STATUS_SUCCESS otherwise.
    hsa_status_t powerRefusalLocked() const;
    void setPowerLocked(amdgpu::power::PowerState state, uint32_t flags);
    bool codeSyncKernelLocked(uint64_t kernelObject) const;
    // Signal events: KFD's auto-reset events and its mailbox page.
    struct Event { bool signaled = false; };
    std::map<uint32_t, Event> events_;
    std::array<uint64_t, 4096> eventPage_{};
    uint32_t nextEvent_ = 1;
    bool dropInterrupts_ = false;
    EventStats eventStats_;
    std::condition_variable eventChanged_;
    void raiseEventLocked(uint32_t id);
};

// Replaces the shared fake with a device built from config. Call before
// hsa_init; the next discover_fake returns the new device.
MAC_HSA_FAKE_EXPORT void setFakeDeviceConfig(const FakeDeviceConfig &config);

// Returns a single FakeConnection. Mirrors discover() in transport.h so the
// runtime can be pointed at the fake backend for host testing.
MAC_HSA_FAKE_EXPORT hsa_status_t discover_fake(std::vector<std::shared_ptr<Connection>> &connections);

// The single shared fake connection (so tests can reach its observability
// accessors after discover_fake). Null before discover_fake is called.
MAC_HSA_FAKE_EXPORT std::shared_ptr<FakeConnection> fakeConnection();

} // namespace mac_hsa
