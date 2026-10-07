#pragma once

#include <hsa/hsa.h>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>
#include <string>
#include "atomic_path.h"
#include "isa_target.h"
#include "power.h"
#include "../abi/amdgpu_dispatch_abi.h"
#include "../abi/amdgpu_aql_abi.h"
#include "../abi/amdgpu_atomic_diagnostics.h"
#include "../abi/amdgpu_atomic_requester.h"

struct mlg_doorbell_gate;

namespace mac_hsa {

constexpr uint64_t kPersistentQueueDriverBuild=187;
constexpr uint64_t kQueueResourceDriverBuild=190;
// First driver build with KFD-backed compute sessions (QueryInfo tag 12).
constexpr uint64_t kComputeSessionDriverBuild=192;
// First driver build with KFD signal events and interrupt waits (selectors
// 86 and 87, dext/sources/session_state.h).
constexpr uint64_t kSignalEventDriverBuild=235;
// First driver build whose queue service (selector 59) reports a GPU memory
// fault of the client's process with its address (four outputs).
constexpr uint64_t kQueueFaultDriverBuild=256;
// First driver build with CodeSync (selector 90): the compute ring's cache
// invalidate alone, no queue and no launch. With it a code-object load only
// marks the connection's code pending, and one sync runs before the next
// doorbell of its queues (flushPendingCodeSync).
constexpr uint64_t kCodeSyncDriverBuild=258;
// First driver build whose BOCopy (selector 48) copies between system pages
// and VRAM by SDMA through TTM's GART windows, with no CPU copy, and takes up
// to kWindowedCopyBytes in one call (kLegacyCopyBytes before).
constexpr uint64_t kWindowedCopyDriverBuild=262;
constexpr uint64_t kLegacyCopyBytes=4ull<<20, kWindowedCopyBytes=16ull<<20;
// First driver build that serves AQLQueueKick (selector 57) synchronously,
// on its delivery thread (session_state.h's MLG_SYNC_KICK_BUILD).
constexpr uint64_t kSyncKickDriverBuild=263;
// First driver build whose KFD queues the client rings itself
// (MLG_DIRECT_DOORBELL_BUILD, dext/sources/doorbell_gate.h): one store to
// the doorbell slice it maps, behind the gate the driver closes before the
// device stops answering.
constexpr uint64_t kDirectDoorbellDriverBuild=264;
// First driver build that lets a client store into the GPU's BARs on its
// submission path (MLG_BAR_WRITES_BUILD, dext/sources/hdp_flush.h): the HDP
// flush page and CPU access to VRAM, behind the client's gate.
constexpr uint64_t kBarWritesDriverBuild=265;
// A queue's doorbell as the client writes it itself: the gate, and the
// queue's doorbell in the mapped slice. Both null for a queue rung through
// kickQueue.
struct DirectDoorbell { mlg_doorbell_gate *gate = nullptr; volatile uint64_t *doorbell = nullptr; };
// BAR writes (kBarWritesDriverBuild, hdp_flush.h), once a connection enabled
// them: the gate every store into the BARs is bracketed under, and the HDP
// flush registers as this process maps them (HSA_AMD_AGENT_INFO_HDP_FLUSH).
struct BarWrites {
    mlg_doorbell_gate *gate = nullptr;
    uint32_t *memFlush = nullptr, *regFlush = nullptr;
    uint64_t visibleVRAM = 0;
};
// A GPU memory fault of this process's GPU work: KFD evicted every queue
// of the process, which never runs again. reason uses the
// hsa_amd_memory_fault_reason_t bits.
struct MemoryFault { uint64_t address = 0; uint32_t reason = 0; };

// Compute-session QueryInfo tag: which path the driver gave this client.
// Legacy: VMID0 buffers and the driver's legacy HQDs (one queue per HQD).
// KFD: the client is a KFD process (its own GPUVM, MES user queues up to
// KFD's per-process limit) and owns a host window in its address space.
constexpr uint64_t kComputeSessionQueryTag = 12;
constexpr uint64_t kComputeSessionQueryWords = 8;
enum class ComputeSessionMode : uint32_t { Unknown = 0, Legacy = 1, KFD = 2 };
enum ComputeSessionWord : unsigned {
    SessionVersion,           // 1
    SessionMode,              // ComputeSessionMode
    SessionQueueSlots,
    SessionProcessID,         // the KFD process's pid (0 on the legacy path)
    SessionWindowBase,        // host window base (0 until set)
    SessionWindowBytes,       // host window size (to reserve while unset)
    SessionGPUVMBase,
    SessionGPUVMLimit,
};
// AQL ring sizes (packets, power of two) the AQLQueueCreate selector accepts
// on every driver build since kPersistentQueueDriverBuild: a driver ABI
// contract, used when the driver does not report its own bounds.
constexpr uint32_t kQueueMinPackets=64, kQueueMaxPackets=4096;

// Compute-topology QueryInfo tag (layout in DeviceTopologyWord). Drivers that
// predate it decline the tag; every field it carries is then "not reported"
// (zero) and the runtime falls back to what the always-present tags imply, or
// leaves the check to the driver.
constexpr uint64_t kTopologyQueryTag = 10;
constexpr uint64_t kTopologyQueryWords = 16;
enum DeviceTopologyWord : unsigned {
    TopologyVersion,          // 1
    TopologyGfxTargetVersion, // KFD node gfx_target_version, e.g. 110000
    TopologySimdPerCU,
    TopologyMaxWavesPerSIMD,
    TopologyGroupSegmentBytes, // LDS available to one workgroup
    TopologyPrivateSegmentBytes, // largest per-work-item scratch a queue accepts
    TopologyScratchSlotsPerCU,
    TopologyQueueSlots,       // AQL queue slots (legacy HQDs) owned by the driver
    TopologyQueuePackets,     // min ring packets | max ring packets << 32
    TopologyL1CacheBytes,
    TopologyL2CacheBytes,
    TopologyL3CacheBytes,
    TopologyMaxClockMHz,
    TopologyTargetFeatures,   // bits 0-1 xnack, bits 2-3 sramecc: 0 unsupported, 1 any, 2 off, 3 on
    TopologyXCCCount,
    TopologyReserved,
};
// Product-name QueryInfo tag: NUL-padded ASCII board name, 8 bytes per word.
constexpr uint64_t kProductNameQueryTag = 11;
constexpr uint64_t kProductNameQueryWords = 8;

struct DeviceSnapshot {
    uint64_t registryID = 0;
    uint64_t build = 0;
    uint64_t stage = 0;
    uint64_t visibleVRAM = 0;
    uint64_t totalVRAM = 0;
    uint32_t gfxMajor = 0, gfxMinor = 0, gfxRevision = 0;
    OriginalAtomicCaps originalAtomicCaps{};
    // KFD gfx_target_version: reported by the driver when it serves the
    // topology tag, otherwise KFD's own mapping of the reported GC IP version.
    uint32_t gfxTargetVersion = 0;
    TargetFeature xnack = TargetFeature::Unsupported, sramecc = TargetFeature::Unsupported;
    // AQL queue slots the driver reserves for runtime queues (the legacy HQDs
    // upstream leaves free). Not reported by older drivers: a ready driver
    // then still guarantees one, because it refuses to start without any.
    uint32_t queueSlots = 0;
    bool queueSlotsReported = false;
    ComputeSessionMode sessionMode = ComputeSessionMode::Unknown;
};

// The agent's ISA, derived from the device snapshot. False when the device
// names no processor this runtime knows; such an agent is not published.
inline bool deviceIsa(const DeviceSnapshot &snapshot, IsaTarget &out) {
    return snapshot.gfxTargetVersion &&
        resolveIsaTarget(snapshot.gfxTargetVersion, snapshot.xnack, snapshot.sramecc, out);
}
inline uint32_t deviceQueueSlots(const DeviceSnapshot &snapshot) {
    return snapshot.queueSlotsReported ? snapshot.queueSlots : 1;
}
// Fills the ISA-related snapshot fields from the GC IP version and, when the
// driver reports them, the topology tag. xnack is Off on this platform: there
// is no recoverable GPU page-fault path (HSA_AMD_SYSTEM_INFO_XNACK_ENABLED).
inline void applyDeviceTopology(DeviceSnapshot &snapshot, const uint64_t *topology) {
    constexpr TargetFeature features[] = {TargetFeature::Unsupported, TargetFeature::Any,
                                          TargetFeature::Off, TargetFeature::On};
    snapshot.gfxTargetVersion = gfxTargetVersionFromGCVersion(
        snapshot.gfxMajor, snapshot.gfxMinor, snapshot.gfxRevision);
    snapshot.xnack = TargetFeature::Off;
    snapshot.sramecc = TargetFeature::Any;
    if (!topology || topology[TopologyVersion] < 1) return;
    if (topology[TopologyGfxTargetVersion] && topology[TopologyGfxTargetVersion] <= UINT32_MAX)
        snapshot.gfxTargetVersion = uint32_t(topology[TopologyGfxTargetVersion]);
    const auto reported = topology[TopologyTargetFeatures];
    if (features[(reported >> 2) & 3] != TargetFeature::Unsupported)
        snapshot.sramecc = features[(reported >> 2) & 3];
    if (topology[TopologyQueueSlots] <= UINT16_MAX) {
        snapshot.queueSlots = uint32_t(topology[TopologyQueueSlots]);
        snapshot.queueSlotsReported = true;
    }
}

inline bool supportsPersistentQueues(const DeviceSnapshot &snapshot) {
    IsaTarget isa;
    return snapshot.build >= kPersistentQueueDriverBuild && deviceIsa(snapshot, isa) &&
        deviceQueueSlots(snapshot) > 0;
}

// AQL dispatch limits shared by every AMDGPU processor, not properties of one
// device: hsa_kernel_dispatch_packet carries 16-bit workgroup and 32-bit grid
// sizes, and the CP launches at most 1024 work-items per workgroup on GFX9
// through GFX12 (the values ROCr reports for every GPU agent).
struct DispatchLimits {
    std::array<uint16_t, 3> workgroupMaxDim;
    uint32_t workgroupMaxSize;
    hsa_dim3_t gridMaxDim;
    uint64_t gridMaxSize;
    uint32_t fbarrierMaxSize;
};
inline constexpr DispatchLimits kDispatchLimits{
    {1024, 1024, 1024}, 1024, {UINT32_MAX, UINT32_MAX, UINT32_MAX}, UINT64_MAX, 32};

struct DeviceBuffer {
    uint64_t handle = 0, address = 0, size = 0;
};
// Device properties. Topology-tag fields are zero when the driver does not
// report them; callers treat zero as "unknown", never as a limit.
struct DeviceProperties {
    uint32_t chipID=0, revision=0, bdf=0, domain=0;
    uint32_t computeUnits=0, shaderEngines=0, arraysPerEngine=0;
    uint64_t timestampFrequency=0;
    uint32_t maxWavesPerCU=0, wavefrontSize=0;
    uint32_t simdPerCU=0, maxWavesPerSIMD=0;
    uint32_t groupSegmentBytes=0, privateSegmentBytes=0, scratchSlotsPerCU=0;
    uint32_t queueMinPackets=0, queueMaxPackets=0;
    uint32_t l1CacheBytes=0, l2CacheBytes=0, l3CacheBytes=0;
    uint32_t maxClockMHz=0, xccCount=0;
    char productName[kProductNameQueryWords * 8 + 1]{};
};
// Applies the topology tag to properties already filled from tag 6.
inline void applyDeviceTopology(DeviceProperties &out, const uint64_t *topology) {
    if (!topology || topology[TopologyVersion] < 1) return;
    const auto word = [&](unsigned index) {
        return topology[index] <= UINT32_MAX ? uint32_t(topology[index]) : 0u;
    };
    out.simdPerCU = word(TopologySimdPerCU);
    out.maxWavesPerSIMD = word(TopologyMaxWavesPerSIMD);
    if (out.simdPerCU && out.maxWavesPerSIMD) out.maxWavesPerCU = out.simdPerCU * out.maxWavesPerSIMD;
    out.groupSegmentBytes = word(TopologyGroupSegmentBytes);
    out.privateSegmentBytes = word(TopologyPrivateSegmentBytes);
    out.scratchSlotsPerCU = word(TopologyScratchSlotsPerCU);
    out.queueMinPackets = uint32_t(topology[TopologyQueuePackets]);
    out.queueMaxPackets = uint32_t(topology[TopologyQueuePackets] >> 32);
    if (out.queueMinPackets > out.queueMaxPackets) out.queueMinPackets = out.queueMaxPackets = 0;
    out.l1CacheBytes = word(TopologyL1CacheBytes);
    out.l2CacheBytes = word(TopologyL2CacheBytes);
    out.l3CacheBytes = word(TopologyL3CacheBytes);
    out.maxClockMHz = word(TopologyMaxClockMHz);
    out.xccCount = word(TopologyXCCCount);
}
inline void applyProductName(DeviceProperties &out, const uint64_t *words) {
    std::memset(out.productName, 0, sizeof(out.productName));
    if (!words) return;
    for (unsigned i = 0; i < kProductNameQueryWords * 8; ++i) {
        const char c = char(words[i / 8] >> ((i % 8) * 8));
        if (!c) break;
        out.productName[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
}
// Raw register-level device spec, one block of 32 dwords read by the driver's
// observer QueryInfo (info tag 8). The driver fills it from the GC_INFO
// discovery table (physical geometry before harvesting) and the live
// harvest/disable-mask/SH-block register reads; header==0 means the driver has
// not resolved the GC IP yet. The field layout travels with the driver build
// that produced it; the HSA runtime never interprets individual words beyond
// the validity check in spec().
constexpr uint64_t kDeviceSpecDwords = 32;
constexpr uint64_t kDeviceSpecDriverBuild = 197; // first driver build serving tag 8
struct SharedBuffer { DeviceBuffer device; void *host = nullptr; uint32_t memoryType = 0; };
// A KFD signal event: an amd_signal_t whose event_mailbox_ptr is mailbox and
// whose event_id is trigger makes the command processor write the mailbox
// and raise an interrupt when it completes a packet naming the signal; the
// driver's interrupt handler signals event id.
struct SignalEvent { uint32_t id = 0, trigger = 0; uint64_t mailbox = 0; };
enum class EventWaitResult { Fired, TimedOut };
struct BufferToken { uint64_t registryID = 0, token[2]{}, size = 0; };
static_assert(sizeof(BufferToken) == 32);

class Connection {
public:
    virtual ~Connection() = default;
    // Code loaded since the last code sync (kCodeSyncDriverBuild): synced
    // before the next doorbell of this connection's queues.
    std::atomic<bool> codeSyncPending{false};
    virtual hsa_status_t read(DeviceSnapshot &snapshot) = 0;
    virtual bool supportsBuffers() const { return false; }
    virtual hsa_status_t properties(DeviceProperties &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    virtual hsa_status_t spec(std::array<uint64_t, kDeviceSpecDwords> &) {
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    // HSA_STATUS_ERROR_MEMORY_FAULT once the process's GPU work faulted;
    // memoryFault then says where.
    virtual hsa_status_t serviceQueue(uint64_t, uint64_t &inactive) { inactive=0;return HSA_STATUS_SUCCESS; }
    virtual bool memoryFault(MemoryFault &) { return false; }
    virtual bool supportsSharedBuffers() const { return false; }
    virtual hsa_status_t sharedMemoryCapacity(uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t memoryCapacity(uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t memoryAvailable(uint64_t &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    virtual hsa_status_t allocateBuffer(uint64_t, DeviceBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    // What this connection holds from the driver and where it was asked for
    // (allocation_census.h); empty when the transport keeps no census.
    virtual std::string memoryReport() { return {}; }
    virtual hsa_status_t freeBuffer(const DeviceBuffer &) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t readBuffer(const DeviceBuffer &, uint64_t, void *, size_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t writeBuffer(const DeviceBuffer &, uint64_t, const void *, size_t) { return HSA_STATUS_ERROR; }
    // Complete code-cache invalidation after upload, before publishing symbols.
    // Data visibility alone does not invalidate reused instruction addresses.
    virtual hsa_status_t invalidateCodeCaches() { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t allocateSharedBuffer(uint64_t, SharedBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t freeSharedBuffer(const SharedBuffer &) { return HSA_STATUS_ERROR; }
    // Diagnostic only: raw SDMA submission claims an exclusive client lease.
    virtual hsa_status_t testSharedAtomicAdd(const SharedBuffer &, uint64_t, int64_t, uint32_t) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t atomicRequesterExperiment(bool,amdgpu::atomic_requester::Snapshot &) {
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    virtual hsa_status_t sharedAtomicDiagnostics(const SharedBuffer &, uint64_t, uint64_t,
        amdgpu::atomic_diag::Snapshot &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // True when every AQL queue slot the device reports is held by a queue
    // this connection created. createQueue and dispatchAQL (whose bounded
    // launch borrows a slot for its duration) then fail with
    // HSA_STATUS_ERROR_OUT_OF_RESOURCES before any driver call.
    virtual bool queueSlotsExhausted() { return false; }
    virtual hsa_status_t createQueue(const SharedBuffer &, const SharedBuffer &, uint32_t, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t kickQueue(uint64_t, uint64_t) { return HSA_STATUS_ERROR; }
    // The queue's own doorbell (kDirectDoorbellDriverBuild), valid until
    // destroyQueue; empty for a queue rung through kickQueue. When its gate
    // is closed (mlg_doorbell_ring returns false) the doorbell is sent
    // through kickQueue, which answers what the device can take.
    virtual DirectDoorbell directDoorbell(uint64_t) { return {}; }
    // A transport whose doorbells are not waited for (kickQueue returns once
    // the doorbell is queued): true, with the highest one, when the driver
    // refused doorbells of @handle because the device was suspending. The
    // queue rings it again on resume, as it does a doorbell refused at once.
    virtual bool takeRefusedKick(uint64_t, uint64_t &) { return false; }
    virtual hsa_status_t destroyQueue(uint64_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t copyBuffers(const DeviceBuffer &, uint64_t, const DeviceBuffer &, uint64_t, size_t) { return HSA_STATUS_ERROR; }
    // The most copyBuffers takes in one call.
    virtual uint64_t maxCopyBytes() { return kLegacyCopyBytes; }
    virtual hsa_status_t exportBuffer(const DeviceBuffer &, BufferToken &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t importBuffer(const BufferToken &, DeviceBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    // Device power (power.h): the driver's power snapshot, and a request
    // (amdgpu::power::Prepare or Resume) answered with the snapshot after
    // it. Callable whatever state the session is in; drivers that predate
    // the protocol decline with HSA_STATUS_ERROR_INVALID_ARGUMENT.
    virtual hsa_status_t powerState(PowerSnapshot &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // Interrupt signals. A connection without them (the legacy path, a
    // driver that predates them) declines with
    // HSA_STATUS_ERROR_INVALID_ARGUMENT and says why in @why.
    virtual hsa_status_t createSignalEvent(SignalEvent &, std::string *why = nullptr) {
        if (why) *why = "the transport has no signal events";
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    virtual hsa_status_t destroySignalEvent(uint32_t) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // A host-side change of a signal: wake whoever sleeps on its event.
    virtual hsa_status_t setSignalEvent(uint32_t) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // Sleep until one of @ids fires or @timeoutMs (1 or more) passes. The
    // calling thread sleeps in the kernel; nothing polls.
    virtual hsa_status_t waitSignalEvents(const uint32_t *, uint32_t, uint32_t, EventWaitResult &) {
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    virtual hsa_status_t requestPower(uint64_t, PowerSnapshot &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // BAR writes (hdp_flush.h). enableBarWrites asks the driver once and
    // maps the gate and the HDP flush page; a driver or session without
    // them declines, saying why in @why. barWrites is what an enabled
    // connection has (gate null before). From then on VRAM is allocated
    // hostable (a VA the CPU can map at) and mapBufferForCPU gives the CPU
    // a write-combined mapping of one at its VA (the driver pins it in the
    // CPU-visible window), undone by freeBuffer.
    virtual hsa_status_t enableBarWrites(BarWrites &, std::string *why = nullptr) {
        if (why) *why = "the transport has no BAR writes";
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    virtual BarWrites barWrites() { return {}; }
    virtual hsa_status_t allocateHostableBuffer(uint64_t, DeviceBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t mapBufferForCPU(const DeviceBuffer &) { return HSA_STATUS_ERROR_INVALID_ARGUMENT; }
    // A bracket found the gate closed: HSA_STATUS_SUCCESS once it may be
    // tried again (the driver held it for a power transition or a reset
    // and reopened it, or the wait should look again), or the device is
    // gone for this process (the gate retired, the session closed, the
    // device lost): every BAR mapping of the connection is retired first
    // (bar_mapping_retire.h), so a store that still follows lands in host
    // memory, and kDeviceLostStatus is returned. Sleeps, longer the more
    // often @attempt says it waited already (a hold can last: a host
    // sleep, a client's low-power prepare); never call it inside a bracket.
    virtual hsa_status_t barWriteWait(unsigned) { return kDeviceLostStatus; }
};

// Discover the installed DriverKit service and initialize the Linux shim
// through its retained UserClient before publishing a GPU agent.
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections);

} // namespace mac_hsa
