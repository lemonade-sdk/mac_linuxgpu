#pragma once

#include "transport.h"
#include "device_init.h"
#include <map>
#include <mutex>
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

    // ---- Connection: dispatch ----
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &request, uint64_t &fence) override;
    hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &request, uint64_t &fence) override;

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
    uint64_t codeSyncs_ = 0, queueCodeSyncs_ = 0;
    FakeDeviceConfig config_;
    DeviceSnapshot snapshot_;
    std::array<uint64_t, kTopologyQueryWords> topology_{};
    std::array<uint64_t, kProductNameQueryWords> productName_{};
    std::array<uint64_t, 10> tag6_{};
    bool queueSlotsExhaustedLocked() const;
    void processQueueLocked(Queue &queue);
    bool codeSyncKernelLocked(uint64_t kernelObject) const;
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
