#include "runtime_state.h"
#include "mac_hsa.h"
#include <hsa/amd_hsa_queue.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <new>
#include <thread>
#include <condition_variable>
#include <system_error>
#include <string>
#include <mutex>
#include <vector>

namespace mac_hsa::detail {
struct RuntimeQueue;
static bool pausedNow(const std::weak_ptr<RuntimeQueue> &weak);

// A GPU memory fault of this process's GPU work: KFD evicted every queue of
// the process and nothing of it runs again, as on Linux. Said once per
// connection, in ROCr's words, and handed to a registered system event
// handler (HSA_AMD_GPU_MEMORY_FAULT_EVENT); each queue's error callback then
// gets HSA_STATUS_ERROR_MEMORY_FAULT. Other processes keep the GPU.
static void reportMemoryFault(const std::shared_ptr<Connection> &connection, hsa_agent_t agent) {
    static std::mutex saidMutex;
    static std::vector<const Connection *> said;
    {
        std::lock_guard lock(saidMutex);
        if (std::find(said.begin(), said.end(), connection.get()) != said.end()) return;
        said.push_back(connection.get());
    }
    MemoryFault fault{};
    const bool known = connection->memoryFault(fault);
    std::string reason;
    const auto add = [&](uint32_t bit, const char *text) {
        if (fault.reason & bit) reason += (reason.empty() ? "" : ", ") + std::string(text);
    };
    add(HSA_AMD_MEMORY_FAULT_PAGE_NOT_PRESENT, "Page not present or supervisor privilege");
    add(HSA_AMD_MEMORY_FAULT_READ_ONLY, "Write access to a read-only page");
    add(HSA_AMD_MEMORY_FAULT_NX, "Execute access to a non-executable page");
    add(HSA_AMD_MEMORY_FAULT_IMPRECISE, "Imprecise address");
    if (reason.empty()) reason = known ? "Unknown" : "Not reported by this driver";
    std::fprintf(stderr, "mac_hsa: Memory access fault by GPU agent %#llx on address %#llx. Reason: %s. "
                 "This process's GPU queues were stopped and run nothing again; other programs "
                 "keep the GPU.\n", (unsigned long long)agent.handle,
                 (unsigned long long)fault.address, reason.c_str());
    hsa_amd_event_t event{};
    event.event_type = HSA_AMD_GPU_MEMORY_FAULT_EVENT;
    event.memory_fault = {agent, fault.address, fault.reason};
    (void)deliverSystemEvent(event);
}
struct RuntimeQueue {
    amd_queue_t hostABI{};
    amd_queue_t *abi=&hostABI;
    std::shared_ptr<Connection> connection;
    SharedBuffer ring, metadata;
    hsa_agent_t agent{};
    uint64_t hardwareHandle=0;
    std::mutex mutex;
    void (*errorCallback)(hsa_status_t,hsa_queue_t *,void *)=nullptr;
    void *errorData=nullptr;
    bool errorDelivered=false;
    bool everKicked=false;
    uint64_t profilingFrequency=0;
    // Device power (power.h): while the device takes no work (or the client
    // prepared for low power), doorbells wait here, the highest value rung,
    // and are rung again once it does. lastKicked is the highest value the
    // driver accepted, what a drain waits for.
    std::atomic<bool> paused{false};
    int64_t pendingDoorbell=-1;
    int64_t lastKicked=-1;
    struct ServiceState {
        std::atomic<bool> stop{false};
        std::mutex waitMutex;
        std::condition_variable changed;
    };
    std::shared_ptr<ServiceState> serviceState;
    std::mutex serviceThreadMutex;
    std::thread serviceThread;
    void stopService() {
        if (serviceState) {serviceState->stop=true;serviceState->changed.notify_all();}
        std::thread retired;
        {
            std::lock_guard lock(serviceThreadMutex);
            retired=std::move(serviceThread);
        }
        if (retired.joinable()) {
            if (retired.get_id()==std::this_thread::get_id()) retired.detach();
            else retired.join();
        }
    }
    void service() {
        hsa_status_t status=HSA_STATUS_SUCCESS;
        bool notify=false;
        {
            std::lock_guard lock(mutex);
            if (!active || !hardwareHandle || errorDelivered) return;
            uint64_t inactive=0;
            status=connection->serviceQueue(hardwareHandle,inactive);
            // A doorbell the driver refused while the device suspended
            // waits, as one refused at once does, and is rung on resume.
            if (uint64_t refused=0; connection->takeRefusedKick(hardwareHandle,refused)) {
                pendingDoorbell=std::max(pendingDoorbell,int64_t(refused));paused=true;
            }
            if (status==kDeviceSuspendedStatus) {paused=true;return;}
            // Taking work again: ring what waited.
            if (status==HSA_STATUS_SUCCESS && paused && !submissionsHeld(connection.get())) {
                paused=false;
                status=replayLocked();
                if (status==kDeviceSuspendedStatus) return;
            }
            if (status!=HSA_STATUS_SUCCESS) {errorDelivered=true;notify=true;}
        }
        // Callbacks may query or destroy this queue; never hold its mutex here.
        if (notify) {
            if (serviceState) serviceState->stop=true;
            if (status==kMemoryFaultStatus) reportMemoryFault(connection,agent);
            else status=deviceStatus(connection,status);
            invalidateGPUSignals(connection);
            if (errorCallback) errorCallback(status,&abi->hsa_queue,errorData);
        }
    }
    // Caller holds mutex. Rings the doorbell that waited, if any.
    hsa_status_t replayLocked() {
        if (pendingDoorbell<0) return HSA_STATUS_SUCCESS;
        const auto status=connection->kickQueue(hardwareHandle,uint64_t(pendingDoorbell));
        if (status==kDeviceSuspendedStatus) {paused=true;return status;}
        if (status==HSA_STATUS_SUCCESS) {lastKicked=std::max(lastKicked,pendingDoorbell);pendingDoorbell=-1;}
        return status;
    }
    void startService(const std::shared_ptr<RuntimeQueue> &self) {
        serviceState=std::make_shared<ServiceState>();
        const std::weak_ptr<RuntimeQueue> weak=self;
        std::lock_guard publication(serviceThreadMutex);
        serviceThread=std::thread([weak,state=serviceState] {
            while (!state->stop.load()) {
                {
                    auto queue=weak.lock();
                    if (!queue) break;
                    queue->service();
                }
                // Paused for device power: poll at a gentler pace.
                const auto pace=std::chrono::milliseconds(pausedNow(weak) ? 20 : 1);
                std::unique_lock lock(state->waitMutex);
                state->changed.wait_for(lock,pace,[&] {return state->stop.load();});
            }
        });
    }
    hsa_status_t inactivate() {
        std::lock_guard lock(mutex);
        if (!active) return HSA_STATUS_SUCCESS;
        if (hardwareHandle) {
            const auto status=connection->destroyQueue(hardwareHandle);
            if (status!=HSA_STATUS_SUCCESS) return status;
            hardwareHandle=0;
        }
        active=false;
        if (serviceState) {serviceState->stop=true;serviceState->changed.notify_all();}
        return HSA_STATUS_SUCCESS;
    }
    void ringDoorbell(int64_t value) {
        bool notify=false;
        hsa_status_t status=HSA_STATUS_SUCCESS;
        // Code loaded since the last sync runs only after this doorbell:
        // its caches are synced first, outside this queue's mutex.
        if (connection && connection->codeSyncPending.load(std::memory_order_seq_cst)) {
            status=flushPendingCodeSync(connection);
            if (status!=HSA_STATUS_SUCCESS) {
                {
                    std::lock_guard lock(mutex);
                    if (!active || errorDelivered) return;
                    errorDelivered=true;
                }
                if (errorCallback) errorCallback(deviceStatus(connection,status),&abi->hsa_queue,errorData);
                return;
            }
        }
        {
            std::lock_guard lock(mutex);
            if (!active || !hardwareHandle || errorDelivered) return;
            everKicked=true;
            // Held for device power: the doorbell waits (the packet is in
            // the ring already) and is rung on resume.
            if (paused || submissionsHeld(connection.get())) {
                pendingDoorbell=std::max(pendingDoorbell,value);paused=true;return;
            }
            status=connection->kickQueue(hardwareHandle,uint64_t(value));
            if (status==kDeviceSuspendedStatus) {pendingDoorbell=std::max(pendingDoorbell,value);paused=true;return;}
            if (status!=HSA_STATUS_SUCCESS) {errorDelivered=true;notify=true;}
            else lastKicked=std::max(lastKicked,value);
        }
        if (!notify) return;
        if (status==kMemoryFaultStatus) {
            reportMemoryFault(connection,agent);
            invalidateGPUSignals(connection);
        } else status=deviceStatus(connection,HSA_STATUS_ERROR);
        if (errorCallback) errorCallback(status,&abi->hsa_queue,errorData);
    }
    std::shared_ptr<Signal> doorbell;
    bool active = true;
    ~RuntimeQueue() {
        stopService();
        if (connection) {
            if (inactivate()!=HSA_STATUS_SUCCESS) return; // driver retains backing until reset
            if (ring.host) connection->freeSharedBuffer(ring);
            if (metadata.host) connection->freeSharedBuffer(metadata);
        } else std::free(abi->hsa_queue.base_address);
    }
};
static bool pausedNow(const std::weak_ptr<RuntimeQueue> &weak) {
    const auto queue=weak.lock();
    return queue && queue->paused.load();
}
namespace {
RetiredQueueSet &queues = *new RetiredQueueSet;
std::shared_ptr<RuntimeQueue> findQueue(const hsa_queue_t *pointer) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return {};
    const auto entry = queues.find(pointer);
    return entry == queues.end() ? nullptr : entry->second;
}
auto index(volatile uint64_t &value) { return std::atomic_ref<uint64_t>(const_cast<uint64_t &>(value)); }
static_assert(offsetof(amd_queue_t, write_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
static_assert(offsetof(amd_queue_t, read_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
}
RetiredQueueSet clearQueues() {
    RetiredQueueSet retired;
    retired.swap(queues);return retired;
}
void stopQueueServices(RetiredQueueSet &retired) {
    for (auto &[pointer,queue]:retired) {(void)pointer;queue->stopService();}
}

// Device power (power.cpp): this connection's runtime queues.
static std::vector<std::shared_ptr<RuntimeQueue>> connectionQueues(const std::shared_ptr<Connection> &connection) {
    std::vector<std::shared_ptr<RuntimeQueue>> found;
    std::lock_guard lock(runtimeMutex);
    for (const auto &[pointer,queue]:queues) {
        (void)pointer;
        if (queue->connection==connection) found.push_back(queue);
    }
    return found;
}
bool drainQueues(const std::shared_ptr<Connection> &connection, std::chrono::steady_clock::time_point deadline) {
    for (const auto &queue:connectionQueues(connection)) {
        for (;;) {
            int64_t kicked;
            {
                std::lock_guard lock(queue->mutex);
                if (!queue->active || queue->errorDelivered) break;
                kicked=queue->lastKicked;
            }
            // Every packet the driver was asked to run has been read.
            if (kicked<0 || int64_t(index(queue->abi->read_dispatch_id).load(std::memory_order_acquire))>kicked)
                break;
            if (std::chrono::steady_clock::now()>=deadline) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    return true;
}
void replayQueues(const std::shared_ptr<Connection> &connection) {
    for (const auto &queue:connectionQueues(connection)) {
        bool notify=false;
        hsa_status_t status=HSA_STATUS_SUCCESS;
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->active || !queue->hardwareHandle || queue->errorDelivered || !queue->paused) continue;
            queue->paused=false;
            status=queue->replayLocked();
            if (status!=HSA_STATUS_SUCCESS && status!=kDeviceSuspendedStatus) {queue->errorDelivered=true;notify=true;}
        }
        if (notify && queue->errorCallback)
            queue->errorCallback(deviceStatus(connection,status),&queue->abi->hsa_queue,queue->errorData);
    }
}
uint32_t pausedQueues(const std::shared_ptr<Connection> &connection) {
    uint32_t paused=0;
    for (const auto &queue:connectionQueues(connection)) paused+=queue->paused.load();
    return paused;
}

namespace {
// AMDHSA kernel descriptor (code object v3+, identical on GFX9 through GFX12).
struct KernelDescriptor {
    uint32_t groupSegmentBytes, privateSegmentBytes, kernargBytes, reserved0;
    int64_t entryOffset;
    uint8_t reserved1[20];
    uint32_t rsrc3, rsrc1, rsrc2;
    uint16_t properties, kernargPreload;
    uint8_t reserved2[4];
};
static_assert(sizeof(KernelDescriptor) == 64);
constexpr uint64_t kCodeSyncEntry = 256;       // instruction offset from the descriptor
constexpr uint64_t kCodeSyncKernarg = 1024;    // kernarg offset in the control buffer
constexpr auto kCodeSyncTimeout = std::chrono::seconds(10);
}

hsa_status_t codeSyncOnRuntimeQueue(const std::shared_ptr<Connection> &connection) {
    using Clock = std::chrono::steady_clock;
    std::shared_ptr<RuntimeQueue> queue;
    {
        std::lock_guard lock(runtimeMutex);
        for (const auto &[pointer, candidate] : queues) {
            (void)pointer;
            if (candidate->connection == connection) { queue = candidate; break; }
        }
    }
    if (!queue) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    mac_hsa::DeviceSnapshot snapshot;
    auto status = connection->read(snapshot);
    if (status != HSA_STATUS_SUCCESS) return status;
    mac_hsa::IsaTarget isa;
    if (!mac_hsa::deviceIsa(snapshot, isa)) return HSA_STATUS_ERROR_INVALID_ISA;
    // Buffers the packet references. A packet that may still execute keeps
    // them: they are released only after its completion is observed.
    struct Resources {
        std::shared_ptr<Connection> connection;
        DeviceBuffer code;
        SharedBuffer control;
        bool retain = false;
        ~Resources() {
            if (retain) return;
            if (control.host) connection->freeSharedBuffer(control);
            if (code.handle) connection->freeBuffer(code);
        }
    } resources{connection, {}, {}};
    status = connection->allocateBuffer(16384, resources.code);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!resources.code.address || resources.code.address % 256 || resources.code.size < 16384)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    // An s_endpgm kernel with no inputs: wave64 (valid on every generation),
    // no kernarg pointer, scratch or LDS. rsrc1 carries only the 16/64-bit
    // denormal mode, as the driver's own code-sync launch uses.
    std::array<uint8_t, kCodeSyncEntry + 4> image{};
    KernelDescriptor descriptor{};
    descriptor.entryOffset = int64_t(kCodeSyncEntry);
    descriptor.rsrc1 = 0xc0000;
    std::memcpy(image.data(), &descriptor, sizeof(descriptor));
    const uint32_t endProgram = mac_hsa::endProgramEncoding(isa);
    std::memcpy(image.data() + kCodeSyncEntry, &endProgram, sizeof(endProgram));
    status = connection->writeBuffer(resources.code, 0, image.data(), image.size());
    if (status != HSA_STATUS_SUCCESS) return status;
    status = connection->allocateSharedBuffer(16384, resources.control);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!resources.control.host || resources.control.device.size < 16384 ||
        resources.control.device.address != reinterpret_cast<uintptr_t>(resources.control.host))
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    std::memset(resources.control.host, 0, 16384);
    auto *completion = new (resources.control.host) mac_hsa::SignalABI{};
    completion->value = 1;
    {
        std::lock_guard lock(queue->mutex);
        if (!queue->active || !queue->hardwareHandle || queue->errorDelivered)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    // Reserve a slot only once the ring has room, so a timeout never leaves
    // a reserved, unwritten packet in the application's queue. Other
    // producers may run concurrently: the write index is claimed by CAS.
    auto &abi = *queue->abi;
    const uint64_t size = abi.hsa_queue.size;
    const auto deadline = Clock::now() + kCodeSyncTimeout;
    uint64_t id = index(abi.write_dispatch_id).load(std::memory_order_acquire);
    for (;;) {
        if (id - index(abi.read_dispatch_id).load(std::memory_order_acquire) < size) {
            if (index(abi.write_dispatch_id).compare_exchange_weak(id, id + 1, std::memory_order_acq_rel,
                                                                   std::memory_order_acquire))
                break;
            continue;
        }
        if (Clock::now() >= deadline) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        id = index(abi.write_dispatch_id).load(std::memory_order_acquire);
    }
    hsa_kernel_dispatch_packet_t packet{};
    packet.header = HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    packet.setup = 1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x = 64; packet.grid_size_x = 64;
    packet.workgroup_size_y = packet.workgroup_size_z = 1;
    packet.grid_size_y = packet.grid_size_z = 1;
    packet.kernel_object = resources.code.address;
    packet.kernarg_address = static_cast<char *>(resources.control.host) + kCodeSyncKernarg;
    packet.completion_signal.handle = resources.control.device.address;
    auto *slot = static_cast<char *>(abi.hsa_queue.base_address) + (id % size) * 64;
    std::memcpy(slot + 4, reinterpret_cast<const char *>(&packet) + 4, 60);
    uint32_t header;
    std::memcpy(&header, &packet, sizeof(header));
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(slot)).store(header, std::memory_order_release);
    // From here the packet may execute: its buffers outlive this call
    // unless its completion is observed.
    resources.retain = true;
    queue->ringDoorbell(int64_t(id));
    auto value = std::atomic_ref<int64_t>(completion->value);
    while (value.load(std::memory_order_acquire) != 0) {
        bool failed;
        {
            std::lock_guard lock(queue->mutex);
            failed = queue->errorDelivered || !queue->active;
        }
        if (failed || Clock::now() >= deadline) return HSA_STATUS_ERROR;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    resources.retain = false;
    return HSA_STATUS_SUCCESS;
}

} // namespace mac_hsa::detail
using namespace mac_hsa::detail;

extern "C" {
hsa_status_t mac_hsa_atomic_requester_experiment(hsa_agent_t agent,uint32_t enable,
    mac_hsa_atomic_requester_experiment_t *out,size_t outSize) {
    std::shared_ptr<mac_hsa::Connection> connection;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (enable>1 || !out || outSize!=sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found=findAgent(agent);
        if (!found || !found->connection) return HSA_STATUS_ERROR_INVALID_AGENT;
        connection=found->connection;
    }
    amdgpu::atomic_requester::Snapshot snapshot{};
    const auto status=connection->atomicRequesterExperiment(enable!=0,snapshot);
    if (amdgpu::atomic_requester::valid(snapshot)) {
        static_assert(sizeof(snapshot)==sizeof(*out));
        std::memcpy(out,&snapshot,sizeof(*out));
    } else if (status==HSA_STATUS_SUCCESS) return HSA_STATUS_ERROR;
    return status;
}
hsa_status_t mac_hsa_shared_atomic_diagnostics(const void *pointer,const hsa_queue_t *q,
    mac_hsa_shared_atomic_diagnostics_t *out,size_t outSize) {
    std::shared_ptr<Allocation> allocation;
    std::shared_ptr<RuntimeQueue> queue;
    uint64_t offset=0;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer || !q || !out || outSize!=sizeof(*out) || (reinterpret_cast<uintptr_t>(pointer)&7))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found=queues.find(q);
        if (found==queues.end()) return HSA_STATUS_ERROR_INVALID_QUEUE;
        queue=found->second;allocation=findAllocation(pointer);
        if (!allocation || !allocation->shared.host || !allocation->connection ||
            allocation->connection!=queue->connection || allocation->base!=allocation->shared.host ||
            allocation->shared.device.address!=reinterpret_cast<uintptr_t>(allocation->base))
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        offset=reinterpret_cast<uintptr_t>(pointer)-reinterpret_cast<uintptr_t>(allocation->base);
        if (offset>allocation->size || 8>allocation->size-offset) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    }
    std::lock_guard lock(queue->mutex);
    if (!queue->active || !queue->hardwareHandle || queue->errorDelivered) return HSA_STATUS_ERROR_INVALID_QUEUE;
    amdgpu::atomic_diag::Snapshot snapshot{};
    const auto status=queue->connection->sharedAtomicDiagnostics(allocation->shared,offset,queue->hardwareHandle,snapshot);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (!amdgpu::atomic_diag::snapshot_valid(snapshot,reinterpret_cast<uintptr_t>(pointer))) return HSA_STATUS_ERROR;
    static_assert(sizeof(snapshot)==sizeof(*out));
    std::memcpy(out,&snapshot,sizeof(*out));return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_queue_create(hsa_agent_t agent,uint32_t size,hsa_queue_type32_t type,
    void (*callback)(hsa_status_t,hsa_queue_t *,void *),void *data,
    uint32_t privateBytes,uint32_t groupBytes,hsa_queue_t **out) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::shared_ptr<mac_hsa::Connection> connection;
    uint64_t id=0;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        *out=nullptr;
        const auto found=findAgent(agent);
        if (!found) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!size || (size&(size-1)) || (type!=HSA_QUEUE_TYPE_SINGLE && type!=HSA_QUEUE_TYPE_MULTI))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!found->connection) return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
        if (lastHandle==UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        connection=found->connection;id=++lastHandle;
    }
    mac_hsa::DeviceSnapshot info{};
    auto status=connection->read(info);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (!mac_hsa::supportsPersistentQueues(info)) return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
    {
        // Ring bounds and per-dispatch LDS/scratch limits come from the
        // device. A limit the driver does not report is left to the driver,
        // which rejects an oversized request before mapping anything.
        mac_hsa::DeviceProperties device{};
        status=connection->properties(device);
        if (status!=HSA_STATUS_SUCCESS && status!=HSA_STATUS_ERROR_INVALID_ARGUMENT) return status;
        const uint32_t minimum=device.queueMinPackets ? device.queueMinPackets : mac_hsa::kQueueMinPackets;
        const uint32_t maximum=device.queueMaxPackets ? device.queueMaxPackets : mac_hsa::kQueueMaxPackets;
        if (size<minimum || size>maximum ||
            (device.privateSegmentBytes && privateBytes!=UINT32_MAX && privateBytes>device.privateSegmentBytes) ||
            (device.groupSegmentBytes && groupBytes!=UINT32_MAX && groupBytes>device.groupSegmentBytes))
            return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
    }
    if (info.build<mac_hsa::kQueueResourceDriverBuild &&
        ((privateBytes && privateBytes!=UINT32_MAX) || (groupBytes && groupBytes!=UINT32_MAX)))
        return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
    // Internal signal acceleration yields its slot before public queue creation.
    // Existing requests finish under the service mutex; no published RMW retries.
    std::shared_ptr<void> signalServiceLease;
    status=reclaimGPUSignalService(connection,&signalServiceLease);
    if (status!=HSA_STATUS_SUCCESS) return status;
    try {
        auto queue=std::make_shared<RuntimeQueue>();
        queue->connection=connection;queue->agent=agent;queue->errorCallback=callback;queue->errorData=data;
        status=connection->allocateSharedBuffer(size_t(size)*64,queue->ring);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,queue->metadata);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!queue->ring.host || !queue->metadata.host || queue->ring.device.size<uint64_t(size)*64 ||
            queue->metadata.device.size<sizeof(amd_queue_t) ||
            queue->ring.device.address!=reinterpret_cast<uintptr_t>(queue->ring.host) ||
            queue->metadata.device.address!=reinterpret_cast<uintptr_t>(queue->metadata.host)) return HSA_STATUS_ERROR;
        std::memset(queue->metadata.host,0,queue->metadata.device.size);
        std::memset(queue->ring.host,0,uint64_t(size)*64);
        queue->abi=static_cast<amd_queue_t *>(queue->metadata.host);
        auto &q=*queue->abi;
        q.hsa_queue.type=type;q.hsa_queue.features=HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
        q.hsa_queue.base_address=queue->ring.host;q.hsa_queue.size=size;q.hsa_queue.id=id;
        q.queue_properties=AMD_QUEUE_PROPERTIES_IS_PTR64;
        q.scratch_wave64_lane_byte_size=privateBytes==UINT32_MAX ? 0 : privateBytes;
        q.read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
        auto *packets=static_cast<uint16_t *>(queue->ring.host);
        for (uint32_t i=0;i<size;++i) packets[size_t(i)*32]=HSA_PACKET_TYPE_INVALID;
        queue->doorbell=std::make_shared<mac_hsa::Signal>();
        const std::weak_ptr<RuntimeQueue> weak=queue;
        queue->doorbell->storeHook=[weak](int64_t value) {if (auto q=weak.lock()) q->ringDoorbell(value);};
        q.hsa_queue.doorbell_signal.handle=reinterpret_cast<uintptr_t>(queue->doorbell->address());
        status=connection->createQueue(queue->ring,queue->metadata,size,queue->hardwareHandle);
        // The driver refused the request (size, LDS or scratch beyond the
        // device's limits) before mapping a queue.
        if (status==HSA_STATUS_ERROR_INVALID_ARGUMENT) return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!queue->hardwareHandle) return HSA_STATUS_ERROR;
        auto *pointer=&q.hsa_queue;
        {
            std::lock_guard lock(runtimeMutex);
            signals.emplace(q.hsa_queue.doorbell_signal.handle,queue->doorbell);
            try {queues.emplace(pointer,queue);}
            catch (...) {signals.erase(q.hsa_queue.doorbell_signal.handle);throw;}
        }
        if (info.build>=mac_hsa::kQueueResourceDriverBuild) {
            try {queue->startService(queue);}
            catch (...) {
                std::lock_guard lock(runtimeMutex);
                queues.erase(pointer);signals.erase(q.hsa_queue.doorbell_signal.handle);throw;
            }
        }
        *out=pointer;return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) {return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
      catch (const std::system_error &) {return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
}
HSA_API_EXPORT hsa_status_t hsa_amd_profiling_set_profiler_enabled(hsa_queue_t *pointer, int enable) {
    std::shared_ptr<RuntimeQueue> queue;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer || (enable != 0 && enable != 1)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found=queues.find(pointer);
        if (found==queues.end()) return HSA_STATUS_ERROR_INVALID_QUEUE;
        queue=found->second;
    }
    std::lock_guard lock(queue->mutex);
    if (!queue->active || queue->errorDelivered) return HSA_STATUS_ERROR_INVALID_QUEUE;
    auto properties=std::atomic_ref<uint32_t>(queue->abi->queue_properties);
    constexpr uint32_t mask=AMD_QUEUE_PROPERTIES_ENABLE_PROFILING;
    if (bool(properties.load(std::memory_order_acquire)&mask)==bool(enable)) return HSA_STATUS_SUCCESS;
    // ROCr AqlQueue::SetProfiling needs suspend/resume only after the first
    // submission. We support the unused-queue case; callers serialize producers.
    if (queue->connection) {
        if (!queue->hardwareHandle || queue->everKicked ||
            index(queue->abi->write_dispatch_id).load() || index(queue->abi->read_dispatch_id).load())
            return HSA_STATUS_ERROR;
        if (enable) {
            mac_hsa::DeviceProperties device{};
            const auto status=queue->connection->properties(device);
            if (status!=HSA_STATUS_SUCCESS || !device.timestampFrequency) return HSA_STATUS_ERROR;
            queue->profilingFrequency=device.timestampFrequency;
        }
    }
    if (enable) properties.fetch_or(mask,std::memory_order_release);
    else properties.fetch_and(~mask,std::memory_order_release);
    return HSA_STATUS_SUCCESS;
}
hsa_status_t mac_hsa_dispatch_timestamps(const hsa_queue_t *pointer,hsa_signal_t completion,
    mac_hsa_dispatch_timestamps_t *out,size_t outSize) {
    if (!out || outSize!=sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto queue=findQueue(pointer);
    if (!queue) return HSA_STATUS_ERROR_INVALID_QUEUE;
    const auto signal=findSignal(completion);
    if (!signal) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    std::lock_guard lock(queue->mutex);
    if (!queue->active || queue->errorDelivered || !queue->hardwareHandle || !queue->profilingFrequency ||
        !(std::atomic_ref<uint32_t>(queue->abi->queue_properties).load(std::memory_order_acquire)&AMD_QUEUE_PROPERTIES_ENABLE_PROFILING))
        return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (!signal->sharedABI || signal->gpuConnection.lock()!=queue->connection || !signal->alive.load())
        return HSA_STATUS_ERROR_INVALID_SIGNAL;
    // CP publishes timestamps before the SYSTEM-release completion decrement.
    // The caller retains a unique signal and cannot reset/reuse it during readout.
    if (signal->value().load(std::memory_order_acquire)!=0 || !signal->alive.load()) return HSA_STATUS_ERROR;
    const auto start=std::atomic_ref<uint64_t>(signal->sharedABI->startTimestamp).load(std::memory_order_relaxed);
    const auto end=std::atomic_ref<uint64_t>(signal->sharedABI->endTimestamp).load(std::memory_order_relaxed);
    if (!start || end<start) return HSA_STATUS_ERROR;
    *out={1,start,end,queue->profilingFrequency,64,MAC_HSA_TIMESTAMP_DOMAIN_GPU};
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_cu_set_mask(const hsa_queue_t *pointer, uint32_t bits, const uint32_t *mask) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (bits % 32 || (bits && !mask)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    // Software queues have no CU affinity; the initial persistent-queue ABI
    // also has no synchronized MQD update operation.
    return HSA_STATUS_ERROR_INVALID_QUEUE;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_set_priority(hsa_queue_t *pointer, hsa_amd_queue_priority_t priority) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (priority < HSA_AMD_QUEUE_PRIORITY_LOW || priority > HSA_AMD_QUEUE_PRIORITY_HIGH) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    return HSA_STATUS_ERROR_INVALID_QUEUE;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_get_info(hsa_queue_t *pointer, hsa_queue_info_attribute_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (!value || (attribute != HSA_AMD_QUEUE_INFO_AGENT && attribute != HSA_AMD_QUEUE_INFO_DOORBELL_ID)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto &queue=queues.at(pointer);
    if (!queue->connection) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (attribute==HSA_AMD_QUEUE_INFO_AGENT) return writeValue(value,queue->agent);
    // The handle is a lifetime nonce, not the physical doorbell index.
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}
hsa_status_t hsa_soft_queue_create(hsa_region_t region, uint32_t size, hsa_queue_type32_t type,
    uint32_t features, hsa_signal_t doorbell, hsa_queue_t **out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    const auto pool = findPool(region.handle);
    if (!pool) return HSA_STATUS_ERROR_INVALID_REGION;
    if (pool->connection) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (!size || (size & (size - 1)) || (type != HSA_QUEUE_TYPE_SINGLE && type != HSA_QUEUE_TYPE_MULTI) ||
        features & ~(HSA_QUEUE_FEATURE_KERNEL_DISPATCH | HSA_QUEUE_FEATURE_AGENT_DISPATCH) || !doorbell.handle)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto signal = signals.find(doorbell.handle);
    if (signal == signals.end()) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    if (lastHandle == UINT64_MAX || uint64_t(size) * 64 > pool->capacity)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto queue = std::make_shared<RuntimeQueue>();
        if (posix_memalign(&queue->abi->hsa_queue.base_address, 4096, (size_t(size) * 64 + 4095) & ~size_t(4095)))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::memset(queue->abi->hsa_queue.base_address, 0, size_t(size) * 64);
        auto packets = static_cast<uint16_t *>(queue->abi->hsa_queue.base_address);
        for (uint32_t i = 0; i < size; ++i) packets[size_t(i) * 32] = HSA_PACKET_TYPE_INVALID;
        queue->abi->hsa_queue.type = type;
        queue->abi->hsa_queue.features = features;
        queue->abi->hsa_queue.doorbell_signal = doorbell;
        queue->abi->hsa_queue.size = size;
        queue->abi->hsa_queue.id = ++lastHandle;
        queue->abi->queue_properties = AMD_QUEUE_PROPERTIES_IS_PTR64;
        queue->abi->read_dispatch_id_field_base_byte_offset = offsetof(amd_queue_t, read_dispatch_id);
        queue->doorbell = signal->second;
        auto pointer = &queue->abi->hsa_queue;
        queues.emplace(pointer, std::move(queue));
        *out = pointer;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_queue_destroy(hsa_queue_t *pointer) {
    const auto queue=findQueue(pointer);
    if (!queue) {
        std::lock_guard lock(runtimeMutex);
        return references ? HSA_STATUS_ERROR_INVALID_QUEUE : HSA_STATUS_ERROR_NOT_INITIALIZED;
    }
    const auto status=queue->inactivate();
    if (status!=HSA_STATUS_SUCCESS) return status;
    {
        std::lock_guard lock(runtimeMutex);
        if (!queues.erase(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        if (queue->connection) {
            queue->doorbell->alive=false;
            signals.erase(queue->abi->hsa_queue.doorbell_signal.handle);
        }
    }
    queue->stopService();
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_queue_inactivate(hsa_queue_t *pointer) {
    const auto queue=findQueue(pointer);
    if (!queue) {
        std::lock_guard lock(runtimeMutex);
        return references ? HSA_STATUS_ERROR_INVALID_QUEUE : HSA_STATUS_ERROR_NOT_INITIALIZED;
    }
    return queue->inactivate();
}

#define QUEUE_LOAD(which, suffix, order) \
uint64_t hsa_queue_load_##which##_index_##suffix(const hsa_queue_t *pointer) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi->which##_dispatch_id).load(order) : 0; \
}
QUEUE_LOAD(read, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(read, scacquire, std::memory_order_acquire)
QUEUE_LOAD(write, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(write, scacquire, std::memory_order_acquire)
#undef QUEUE_LOAD
#define QUEUE_STORE(which, suffix, order) \
void hsa_queue_store_##which##_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (queue) index(queue->abi->which##_dispatch_id).store(value, order); \
}
QUEUE_STORE(read, relaxed, std::memory_order_relaxed)
QUEUE_STORE(read, screlease, std::memory_order_release)
QUEUE_STORE(write, relaxed, std::memory_order_relaxed)
QUEUE_STORE(write, screlease, std::memory_order_release)
#undef QUEUE_STORE
#define QUEUE_RMW(suffix, order, failure) \
uint64_t hsa_queue_add_write_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi->write_dispatch_id).fetch_add(value, order) : 0; \
} \
uint64_t hsa_queue_cas_write_index_##suffix(const hsa_queue_t *pointer, uint64_t expected, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (!queue) return 0; \
    index(queue->abi->write_dispatch_id).compare_exchange_strong(expected, value, order, failure); \
    return expected; \
}
QUEUE_RMW(relaxed, std::memory_order_relaxed, std::memory_order_relaxed)
QUEUE_RMW(scacquire, std::memory_order_acquire, std::memory_order_acquire)
QUEUE_RMW(screlease, std::memory_order_release, std::memory_order_relaxed)
QUEUE_RMW(scacq_screl, std::memory_order_acq_rel, std::memory_order_acquire)
#undef QUEUE_RMW
} // extern C
