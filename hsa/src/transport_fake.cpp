#include "transport_fake.h"
#include "signal_state.h"
#include <hsa/amd_hsa_queue.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mac_hsa {
namespace {
// The single shared fake connection, so tests can reach observability
// accessors after discover_fake. Set by discover_fake.
std::mutex &g_fakeMutex = *new std::mutex;
std::shared_ptr<FakeConnection> &g_fake = *new std::shared_ptr<FakeConnection>;
}

FakeConnection::FakeConnection() : FakeConnection(FakeDeviceConfig{}) {}

FakeConnection::FakeConnection(const FakeDeviceConfig &config) : config_(config) {
    power_.words[amdgpu::power::Version] = amdgpu::power::kVersion;
    power_.words[amdgpu::power::Generation] = 1;
    power_.words[amdgpu::power::Flags] = amdgpu::power::VRAMPreserved;
    // The QueryInfo payloads a driver would return for this device; read()
    // and properties() decode them with the same helpers as the IOKit
    // transport, so the runtime sees exactly what a real driver reports.
    tag6_ = {config.chipID, config.revision, config.bdf, config.domain, config.computeUnits,
             config.shaderEngines, config.arraysPerEngine, config.timestampFrequency,
             uint64_t(config.simdPerCU) * config.maxWavesPerSIMD, config.wavefrontSize};
    const auto encode = [](TargetFeature feature) { return uint64_t(feature); };
    topology_[TopologyVersion] = 1;
    topology_[TopologyGfxTargetVersion] = config.gfxTargetVersion ? config.gfxTargetVersion :
        gfxTargetVersionFromGCVersion(config.gcMajor, config.gcMinor, config.gcRevision);
    topology_[TopologySimdPerCU] = config.simdPerCU;
    topology_[TopologyMaxWavesPerSIMD] = config.maxWavesPerSIMD;
    topology_[TopologyGroupSegmentBytes] = config.groupSegmentBytes;
    topology_[TopologyPrivateSegmentBytes] = config.privateSegmentBytes;
    topology_[TopologyScratchSlotsPerCU] = config.scratchSlotsPerCU;
    topology_[TopologyQueueSlots] = config.queueSlots;
    topology_[TopologyQueuePackets] = config.queueMinPackets | (uint64_t(config.queueMaxPackets) << 32);
    topology_[TopologyL1CacheBytes] = config.l1CacheBytes;
    topology_[TopologyL2CacheBytes] = config.l2CacheBytes;
    topology_[TopologyL3CacheBytes] = config.l3CacheBytes;
    topology_[TopologyMaxClockMHz] = config.maxClockMHz;
    topology_[TopologyTargetFeatures] = encode(TargetFeature::Off) | (encode(config.sramecc) << 2);
    topology_[TopologyXCCCount] = config.xccCount;
    for (size_t i = 0; i < config.productName.size() && i < productName_.size() * 8; ++i)
        productName_[i / 8] |= uint64_t(uint8_t(config.productName[i])) << ((i % 8) * 8);

    snapshot_.registryID = 0x1000;
    snapshot_.build = config.build;
    snapshot_.stage = 15; // fully brought up
    snapshot_.visibleVRAM = config.visibleVRAM;
    snapshot_.totalVRAM = config.totalVRAM;
    snapshot_.gfxMajor = config.gcMajor;
    snapshot_.gfxMinor = config.gcMinor;
    snapshot_.gfxRevision = config.gcRevision;
    snapshot_.sessionMode = config.sessionMode;
    applyDeviceTopology(snapshot_, config.topologyReported ? topology_.data() : nullptr);
}

hsa_status_t FakeConnection::read(DeviceSnapshot &out) {
    std::lock_guard lock(mutex_);
    out = snapshot_;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::properties(DeviceProperties &out) {
    std::lock_guard lock(mutex_);
    DeviceProperties properties{};
    properties.chipID = uint32_t(tag6_[0]); properties.revision = uint32_t(tag6_[1]);
    properties.bdf = uint32_t(tag6_[2]); properties.domain = uint32_t(tag6_[3]);
    properties.computeUnits = uint32_t(tag6_[4]); properties.shaderEngines = uint32_t(tag6_[5]);
    properties.arraysPerEngine = uint32_t(tag6_[6]); properties.timestampFrequency = tag6_[7];
    properties.maxWavesPerCU = uint32_t(tag6_[8]); properties.wavefrontSize = uint32_t(tag6_[9]);
    if (config_.topologyReported) applyDeviceTopology(properties, topology_.data());
    applyProductName(properties, config_.productName.empty() ? nullptr : productName_.data());
    out = properties;
    return HSA_STATUS_SUCCESS;
}

bool FakeConnection::queueSlotsExhaustedLocked() const {
    return queues_.size() >= deviceQueueSlots(snapshot_);
}

bool FakeConnection::queueSlotsExhausted() {
    std::lock_guard lock(mutex_);
    return queueSlotsExhaustedLocked();
}

hsa_status_t FakeConnection::allocateBuffer(uint64_t size, DeviceBuffer &out) {
    std::lock_guard lock(mutex_);
    if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
    if (!size || size > (64ull << 30)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    auto *host = std::calloc(1, size);
    if (!host) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    const auto handle = nextBufferHandle_++;
    // VRAM buffers have a "device address" that is distinct from the host
    // pointer (models the GPU VA space). Use an offset well above the host
    // range so the runtime's address-collision checks behave realistically.
    const auto deviceAddress = reinterpret_cast<uintptr_t>(host) | (1ull << 40);
    buffers_.emplace(handle, Buffer{host, size, false});
    out = {handle, deviceAddress, size};
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::freeBuffer(const DeviceBuffer &buffer) {
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(buffer.handle);
    if (it == buffers_.end() || it->second.shared) return HSA_STATUS_ERROR;
    std::free(it->second.host);
    buffers_.erase(it);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::readBuffer(const DeviceBuffer &buffer, uint64_t offset, void *dst, size_t size) {
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(buffer.handle);
    if (it == buffers_.end() || offset + size > it->second.size) return HSA_STATUS_ERROR;
    std::memcpy(dst, static_cast<char *>(it->second.host) + offset, size);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *src, size_t size) {
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(buffer.handle);
    if (it == buffers_.end() || offset + size > it->second.size) return HSA_STATUS_ERROR;
    std::memcpy(static_cast<char *>(it->second.host) + offset, src, size);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::copyBuffers(const DeviceBuffer &dst, uint64_t dstOffset,
                                         const DeviceBuffer &src, uint64_t srcOffset, size_t size) {
    std::lock_guard lock(mutex_);
    const auto dstIt = buffers_.find(dst.handle);
    const auto srcIt = buffers_.find(src.handle);
    if (dstIt == buffers_.end() || srcIt == buffers_.end()) return HSA_STATUS_ERROR;
    if (dstOffset + size > dstIt->second.size || srcOffset + size > srcIt->second.size)
        return HSA_STATUS_ERROR;
    std::memcpy(static_cast<char *>(dstIt->second.host) + dstOffset,
                static_cast<char *>(srcIt->second.host) + srcOffset, size);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::invalidateCodeCaches() {
    // Nothing to invalidate on the host, but the driver's code-sync is a
    // bounded launch that borrows a queue slot: with every slot held it is
    // refused before anything is submitted (not a fault).
    std::lock_guard lock(mutex_);
    // A driver with CodeSync (kCodeSyncDriverBuild) launches nothing, so
    // it needs no slot.
    if (config_.build < kCodeSyncDriverBuild && queueSlotsExhaustedLocked())
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    ++codeSyncs_;
    kicksAtCodeSync_ = 0;
    for (const auto &[handle, queue] : queues_) { (void)handle; kicksAtCodeSync_ += queue.kicks; }
    return HSA_STATUS_SUCCESS;
}
uint64_t FakeConnection::kicksAtLastCodeSync() const {
    std::lock_guard lock(mutex_);
    return kicksAtCodeSync_;
}

hsa_status_t FakeConnection::exportBuffer(const DeviceBuffer &buffer, BufferToken &token) {
    std::lock_guard lock(mutex_);
    if (buffers_.find(buffer.handle) == buffers_.end()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    token = {snapshot_.registryID, {buffer.handle, 0}, buffer.size};
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::importBuffer(const BufferToken &token, DeviceBuffer &out) {
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(token.token[0]);
    if (it == buffers_.end()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    out = {it->first, reinterpret_cast<uintptr_t>(it->second.host) | (1ull << 40), it->second.size};
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::sharedMemoryCapacity(uint64_t &capacity) {
    capacity = 16ull << 30;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::memoryCapacity(uint64_t &capacity) {
    capacity = snapshot_.totalVRAM;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::memoryAvailable(uint64_t &available) {
    uint64_t used = 0;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[handle, buffer] : buffers_)
            if (!buffer.shared) used += buffer.size;
    }
    available = (used < snapshot_.totalVRAM) ? snapshot_.totalVRAM - used : 0;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::allocateSharedBuffer(uint64_t size, SharedBuffer &out) {
    std::lock_guard lock(mutex_);
    if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
    if (!size || size > (64ull << 30)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    auto *host = std::calloc(1, size);
    if (!host) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    const auto handle = nextBufferHandle_++;
    // Shared (GTT) buffers have CPU and GPU addresses identical (the runtime
    // checks buffer.address == host pointer for shared buffers).
    buffers_.emplace(handle, Buffer{host, size, true});
    SharedBuffer sharedOut;
    sharedOut.device.handle = handle;
    sharedOut.device.address = reinterpret_cast<uintptr_t>(host);
    sharedOut.device.size = size;
    sharedOut.host = host;
    sharedOut.memoryType = 0;
    out = sharedOut;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::freeSharedBuffer(const SharedBuffer &buffer) {
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(buffer.device.handle);
    if (it == buffers_.end() || !it->second.shared) return HSA_STATUS_ERROR;
    std::free(it->second.host);
    buffers_.erase(it);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::testSharedAtomicAdd(const SharedBuffer &buffer, uint64_t offset,
                                                 int64_t value, uint32_t result) {
    (void)result;
    std::lock_guard lock(mutex_);
    const auto it = buffers_.find(buffer.device.handle);
    if (it == buffers_.end() || offset + 8 > it->second.size) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    auto *word = reinterpret_cast<std::atomic<int64_t> *>(static_cast<char *>(it->second.host) + offset);
    (void)word->fetch_add(value, std::memory_order_relaxed);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::createQueue(const SharedBuffer &ring, const SharedBuffer &metadata,
                                         uint32_t size, uint64_t &handle) {
    (void)ring; (void)metadata;
    std::lock_guard lock(mutex_);
    if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
    if (!size) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    // The driver owns deviceQueueSlots() hardware queues; a create beyond
    // them fails before anything is mapped.
    if (queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    handle = nextQueueHandle_++;
    queues_.emplace(handle, Queue{size, 0, 0, true, ring.host, metadata.host});
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::kickQueue(uint64_t handle, uint64_t doorbell) {
    std::lock_guard lock(mutex_);
    if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
    const auto it = queues_.find(handle);
    if (it == queues_.end() || !it->second.active) return HSA_STATUS_ERROR;
    if (faulted_) return kMemoryFaultStatus;
    it->second.doorbell = doorbell;
    it->second.kicks++;
    processQueueLocked(it->second);
    return HSA_STATUS_SUCCESS;
}

// True when kernelObject is a kernel descriptor in VRAM whose entry
// instruction is the device's s_endpgm: the runtime's code-sync kernel.
bool FakeConnection::codeSyncKernelLocked(uint64_t kernelObject) const {
    IsaTarget isa;
    if (!deviceIsa(snapshot_, isa)) return false;
    for (const auto &[handle, buffer] : buffers_) {
        (void)handle;
        if (buffer.shared) continue;
        const uint64_t base = reinterpret_cast<uintptr_t>(buffer.host) | (1ull << 40);
        if (kernelObject < base || kernelObject - base + 64 > buffer.size) continue;
        const auto *descriptor = static_cast<const uint8_t *>(buffer.host) + (kernelObject - base);
        int64_t entry = 0;
        std::memcpy(&entry, descriptor + 16, sizeof(entry));
        if (entry < 64 || uint64_t(entry) + 4 > buffer.size - (kernelObject - base)) return false;
        uint32_t instruction = 0;
        std::memcpy(&instruction, descriptor + entry, sizeof(instruction));
        return instruction == endProgramEncoding(isa);
    }
    return false;
}

// A minimal packet processor: it executes the runtime's code-sync kernel
// (completing its signal) and stops at the first packet it cannot run, as
// a CP waits on a packet that is not yet valid.
void FakeConnection::processQueueLocked(Queue &queue) {
    if (!queue.ring || !queue.metadata || !queue.size) return;
    auto *abi = static_cast<amd_queue_t *>(queue.metadata);
    auto read = std::atomic_ref<uint64_t>(const_cast<uint64_t &>(abi->read_dispatch_id));
    const auto write = std::atomic_ref<uint64_t>(const_cast<uint64_t &>(abi->write_dispatch_id))
        .load(std::memory_order_acquire);
    for (uint64_t id = read.load(std::memory_order_acquire); id < write; ++id) {
        auto *packet = reinterpret_cast<hsa_kernel_dispatch_packet_t *>(
            static_cast<char *>(queue.ring) + (id % queue.size) * 64);
        auto header = std::atomic_ref<uint16_t>(packet->header);
        if ((header.load(std::memory_order_acquire) & 0xff) != HSA_PACKET_TYPE_KERNEL_DISPATCH ||
            !codeSyncKernelLocked(packet->kernel_object)) break;
        ++queueCodeSyncs_;
        if (packet->completion_signal.handle) {
            auto *signal = reinterpret_cast<SignalABI *>(packet->completion_signal.handle);
            std::atomic_ref<int64_t>(signal->value).fetch_sub(1, std::memory_order_release);
            if (signal->eventMailbox) {
                *reinterpret_cast<volatile uint64_t *>(signal->eventMailbox) = signal->eventID;
                if (dropInterrupts_) ++eventStats_.dropped;
                else { ++eventStats_.interrupts; raiseEventLocked(signal->eventID); }
            }
        }
        header.store(HSA_PACKET_TYPE_INVALID, std::memory_order_release);
        read.store(id + 1, std::memory_order_release);
    }
}

hsa_status_t FakeConnection::destroyQueue(uint64_t handle) {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(handle);
    if (it == queues_.end()) return HSA_STATUS_ERROR;
    it->second.active = false;
    queues_.erase(it);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::serviceQueue(uint64_t handle, uint64_t &inactive) {
    std::lock_guard lock(mutex_);
    if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
    const auto it = queues_.find(handle);
    if (it == queues_.end()) return HSA_STATUS_ERROR;
    inactive = 0; // queue is "active" (not idle)
    return faulted_ ? kMemoryFaultStatus : HSA_STATUS_SUCCESS;
}

bool FakeConnection::memoryFault(MemoryFault &out) {
    std::lock_guard lock(mutex_);
    if (!faulted_) return false;
    out = fault_;
    return true;
}

void FakeConnection::injectMemoryFault(uint64_t address, uint32_t reason) {
    std::lock_guard lock(mutex_);
    faulted_ = true;
    fault_ = {address, reason};
}

hsa_status_t FakeConnection::dispatchAQL(const amdgpu::AQLDispatchRequest &request, uint64_t &fence) {
    {
        std::lock_guard lock(mutex_);
        if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
        // The bounded launch borrows a hardware queue slot for its duration.
        if (queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        lastAQL_ = request;
        hasLastAQL_ = true;
        ++aqlDispatches_;
    }
    // Simulate the GPU signal-op kernel: when the dispatch matches the
    // signal-context one-shot executor (kernarg 36 bytes, single workgroup of
    // 32, two shared buffers = result + arena), perform the atomic on the
    // signal address in the shared arena and write the old value to result.
    // This makes GPU-backed signals functional on the host so the runtime's
    // signal state machine is exercisable end-to-end.
    if (request.kernargBytes == 36 && request.groups[0] == 1 && request.threads[0] == 32 &&
        request.threads[1] == 1 && request.threads[2] == 1 &&
        request.buffers[0] && request.buffers[1]) {
        auto kernarg = buffers_.find(request.kernargHandle);
        auto result = buffers_.find(request.buffers[0]);
        if (kernarg != buffers_.end() && result != buffers_.end() && result->second.shared) {
            const auto *args = static_cast<const uint8_t *>(kernarg->second.host);
            uint64_t signalAddr = 0, resultAddr = 0;
            int64_t value = 0, compare = 0;
            uint32_t op = 0;
            std::memcpy(&signalAddr, args, 8);
            std::memcpy(&resultAddr, args + 8, 8);
            std::memcpy(&value, args + 16, 8);
            std::memcpy(&compare, args + 24, 8);
            std::memcpy(&op, args + 32, 4);
            auto *signal = reinterpret_cast<std::atomic<int64_t> *>(signalAddr);
            auto *out = reinterpret_cast<int64_t *>(resultAddr);
            int64_t old = 0;
            switch (op) {
            case 1: old = signal->exchange(value, std::memory_order_release); break;
            case 2: old = signal->fetch_add(value, std::memory_order_relaxed); break;
            case 3: old = signal->fetch_sub(value, std::memory_order_relaxed); break;
            case 4: old = signal->fetch_and(value, std::memory_order_relaxed); break;
            case 5: old = signal->fetch_or(value, std::memory_order_relaxed); break;
            case 6: old = signal->fetch_xor(value, std::memory_order_relaxed); break;
            case 7: old = signal->exchange(value, std::memory_order_relaxed); break;
            case 8: signal->compare_exchange_strong(compare, value, std::memory_order_relaxed, std::memory_order_relaxed); old = compare; break;
            default: return HSA_STATUS_ERROR;
            }
            *out = old;
        }
    }
    fence = 0; // zero = success (the signal executor checks `completion` == 0)
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::dispatch(const amdgpu::ComputeDispatchRequest &request, uint64_t &fence) {
    {
        std::lock_guard lock(mutex_);
        if (const auto refused = powerRefusalLocked(); refused != HSA_STATUS_SUCCESS) return refused;
        // Selector 51 is a bounded launch that borrows a queue slot; with
        // every slot held it is refused before submission (not a fault).
        if (queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        lastCompute_ = request;
        hasLastCompute_ = true;
    }
    fence = 1;
    return HSA_STATUS_SUCCESS;
}

// ---- device power: what the driver's power state machine reports ----
hsa_status_t FakeConnection::powerRefusalLocked() const {
    switch (power_.state()) {
    case amdgpu::power::PowerState::Active: return HSA_STATUS_SUCCESS;
    case amdgpu::power::PowerState::Lost: return HSA_STATUS_ERROR; // the session was closed
    default: return kDeviceSuspendedStatus;
    }
}

void FakeConnection::setPowerLocked(amdgpu::power::PowerState state, uint32_t flags) {
    using namespace amdgpu::power;
    if (state != power_.state()) {
        ++power_.words[Generation];
        if (state == PowerState::Lost) ++power_.words[Losses];
    }
    power_.words[State] = uint64_t(state);
    power_.words[Flags] = flags;
}

void FakeConnection::setPowerState(amdgpu::power::PowerState state, bool vramPreserved) {
    std::lock_guard lock(mutex_);
    uint32_t flags = vramPreserved && state != amdgpu::power::PowerState::Lost ? amdgpu::power::VRAMPreserved : 0;
    if (state == amdgpu::power::PowerState::Suspended && vramPreserved) flags |= amdgpu::power::KFDQuiesced;
    setPowerLocked(state, flags);
}

uint64_t FakeConnection::powerRequests(uint64_t op) const {
    std::lock_guard lock(mutex_);
    return op < 4 ? powerRequests_[op] : 0;
}

hsa_status_t FakeConnection::powerState(PowerSnapshot &out) {
    std::lock_guard lock(mutex_);
    out = power_;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::requestPower(uint64_t op, PowerSnapshot &out) {
    using namespace amdgpu::power;
    std::lock_guard lock(mutex_);
    if (op != Prepare && op != Resume) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    ++powerRequests_[op];
    auto &holds = power_.words[Holds];
    if (op == Prepare) {
        ++holds;
        if (power_.state() == amdgpu::power::PowerState::Active) {
            setPowerLocked(amdgpu::power::PowerState::Suspended, VRAMPreserved | KFDQuiesced);
            ++power_.words[Quiesces];
        }
    } else if (holds) {
        --holds;
        if (!holds && power_.state() == amdgpu::power::PowerState::Suspended)
            setPowerLocked(amdgpu::power::PowerState::Active, VRAMPreserved);
    }
    out = power_;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::scalar(uint32_t selector, std::span<const uint64_t> input,
                                    std::span<uint64_t> output) {
    (void)input;
    std::lock_guard lock(mutex_);
    // Model the device-init RPC selectors the fake device needs. The fake is
    // already "brought up" (stage 15), so the init path that joins an
    // initialized peer (device_init.cpp stage==15 branch) is what we serve.
    if (selector == 43) { // RuntimeBuild
        if (output.size() < 3) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        output[0] = 0x414d444750554142ull; // "ABUPGMAD" (the mac_amdgpu magic)
        output[1] = 1;
        output[2] = snapshot_.build;
        return HSA_STATUS_SUCCESS;
    }
    if (selector == 1) { // GetIdentity
        if (output.size() < 7) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        output[0] = 0; output[1] = 0; output[2] = 0;
        output[3] = 0x1002; // AMD vendor
        output[4] = config_.chipID;
        output[5] = 0x030000; // display controller class
        output[6] = config_.revision;
        return HSA_STATUS_SUCCESS;
    }
    if (selector == 21) { // QueryInfo
        // The tag is input[0]. Serve the tags the runtime reads.
        const auto tag = input.empty() ? UINT64_MAX : input[0];
        const auto reply = [&](std::span<const uint64_t> words) {
            if (output.size() != words.size()) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            std::copy(words.begin(), words.end(), output.begin());
            return HSA_STATUS_SUCCESS;
        };
        const auto packed = [](uint32_t major, uint32_t minor, uint32_t revision) {
            return uint64_t(major) << 16 | uint64_t(minor) << 8 | revision;
        };
        switch (tag) {
        case 1: return reply(std::array<uint64_t, 3>{config_.gcMajor, config_.gcMinor, config_.gcRevision});
        case 2: return reply(std::array<uint64_t, 2>{config_.visibleVRAM, config_.totalVRAM});
        case 3: return reply(std::array<uint64_t, 4>{packed(config_.gcMajor, config_.gcMinor, config_.gcRevision),
                                                     config_.sdmaIP, config_.mmhubIP, config_.mp0IP});
        case 4: return reply(std::array<uint64_t, 1>{snapshot_.stage});
        case 5: {
            std::array<uint64_t, 15> accounting{};
            accounting[0] = 1; accounting[1] = 1; accounting[10] = config_.totalVRAM;
            return reply(accounting);
        }
        case 6: return reply(tag6_);
        case kTopologyQueryTag:
            return config_.topologyReported ? reply(topology_) : HSA_STATUS_ERROR;
        case kProductNameQueryTag:
            return config_.productName.empty() ? HSA_STATUS_ERROR : reply(productName_);
        default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        }
    }
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}

hsa_status_t FakeConnection::prepareFirmware(const std::vector<FirmwareFile> &files) {
    std::lock_guard lock(mutex_);
    preparedFirmware_ = files;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::uploadFirmware(const FirmwareFile &file) {
    std::lock_guard lock(mutex_);
    uploadedFirmware_.push_back(file.name);
    return HSA_STATUS_SUCCESS;
}

void FakeConnection::waitAfterReset() {
    // No-op: the fake device is already up.
}

uint64_t FakeConnection::kickCount(uint64_t handle) const {
    std::lock_guard lock(mutex_);
    const auto it = queues_.find(handle);
    return it == queues_.end() ? 0 : it->second.kicks;
}

size_t FakeConnection::queueCount() const {
    std::lock_guard lock(mutex_);
    return queues_.size();
}

size_t FakeConnection::bufferCount() const {
    std::lock_guard lock(mutex_);
    return buffers_.size();
}

uint64_t FakeConnection::aqlDispatchCount() const {
    std::lock_guard lock(mutex_);
    return aqlDispatches_;
}

uint64_t FakeConnection::codeSyncCount() const {
    std::lock_guard lock(mutex_);
    return codeSyncs_;
}

uint64_t FakeConnection::queueCodeSyncCount() const {
    std::lock_guard lock(mutex_);
    return queueCodeSyncs_;
}

const amdgpu::AQLDispatchRequest *FakeConnection::lastAQL() const {
    std::lock_guard lock(mutex_);
    return hasLastAQL_ ? &lastAQL_ : nullptr;
}

const amdgpu::ComputeDispatchRequest *FakeConnection::lastCompute() const {
    std::lock_guard lock(mutex_);
    return hasLastCompute_ ? &lastCompute_ : nullptr;
}

hsa_status_t discover_fake(std::vector<std::shared_ptr<Connection>> &connections) {
    std::lock_guard lock(g_fakeMutex);
    if (!g_fake) g_fake = std::make_shared<FakeConnection>();
    // Like the IOKit discovery: a device that names no known ISA is skipped.
    DeviceSnapshot snapshot;
    IsaTarget isa;
    if (g_fake->read(snapshot) == HSA_STATUS_SUCCESS && deviceIsa(snapshot, isa))
        connections.push_back(g_fake);
    return HSA_STATUS_SUCCESS;
}

void setFakeDeviceConfig(const FakeDeviceConfig &config) {
    std::lock_guard lock(g_fakeMutex);
    g_fake = std::make_shared<FakeConnection>(config);
}

std::shared_ptr<FakeConnection> fakeConnection() {
    std::lock_guard lock(g_fakeMutex);
    return g_fake;
}

} // namespace mac_hsa

namespace mac_hsa {
hsa_status_t FakeConnection::createSignalEvent(SignalEvent &out, std::string *why) {
    std::lock_guard lock(mutex_);
    if (!config_.signalEvents) {
        if (why) *why = "the fake device serves no signal events";
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (events_.size() >= eventPage_.size() - 1) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    while (events_.count(nextEvent_) || !nextEvent_ || nextEvent_ >= eventPage_.size())
        nextEvent_ = nextEvent_ + 1 >= eventPage_.size() ? 1 : nextEvent_ + 1;
    const uint32_t id = nextEvent_++;
    events_[id] = {};
    eventPage_[id] = UINT64_MAX;
    out = {id, id, uint64_t(reinterpret_cast<uintptr_t>(&eventPage_[id]))};
    ++eventStats_.created;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t FakeConnection::destroySignalEvent(uint32_t id) {
    std::lock_guard lock(mutex_);
    if (!events_.erase(id)) return HSA_STATUS_ERROR;
    eventChanged_.notify_all();
    return HSA_STATUS_SUCCESS;
}

void FakeConnection::raiseEventLocked(uint32_t id) {
    const auto it = events_.find(id);
    if (it == events_.end()) return;
    it->second.signaled = true;
    eventChanged_.notify_all();
}

hsa_status_t FakeConnection::setSignalEvent(uint32_t id) {
    std::lock_guard lock(mutex_);
    if (!events_.count(id)) return HSA_STATUS_ERROR;
    ++eventStats_.sets;
    raiseEventLocked(id);
    return HSA_STATUS_SUCCESS;
}

// As KFD's WAIT_EVENTS (any): an auto-reset event that fired before the
// wait completes it at once and is consumed.
hsa_status_t FakeConnection::waitSignalEvents(const uint32_t *ids, uint32_t count, uint32_t timeoutMs,
                                              EventWaitResult &result) {
    std::unique_lock lock(mutex_);
    ++eventStats_.waits;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto it = events_.find(ids[i]);
            if (it == events_.end()) return HSA_STATUS_ERROR;	// destroyed: KFD's -EIO
            if (it->second.signaled) {
                it->second.signaled = false;
                ++eventStats_.firedWaits;
                result = EventWaitResult::Fired;
                return HSA_STATUS_SUCCESS;
            }
        }
        if (eventChanged_.wait_until(lock, deadline) == std::cv_status::timeout) {
            ++eventStats_.timedOutWaits;
            result = EventWaitResult::TimedOut;
            return HSA_STATUS_SUCCESS;
        }
    }
}

void FakeConnection::completeSignal(uint64_t signalHandle) {
    std::lock_guard lock(mutex_);
    auto *signal = reinterpret_cast<SignalABI *>(signalHandle);
    std::atomic_ref<int64_t>(signal->value).fetch_sub(1, std::memory_order_release);
    if (!signal->eventMailbox) return;
    *reinterpret_cast<volatile uint64_t *>(signal->eventMailbox) = signal->eventID;
    if (dropInterrupts_) { ++eventStats_.dropped; return; }
    ++eventStats_.interrupts;
    raiseEventLocked(signal->eventID);
}

void FakeConnection::dropInterrupts(bool drop) {
    std::lock_guard lock(mutex_);
    dropInterrupts_ = drop;
}

FakeConnection::EventStats FakeConnection::eventStats() const {
    std::lock_guard lock(mutex_);
    auto stats = eventStats_;
    stats.live = events_.size();
    return stats;
}
} // namespace mac_hsa
