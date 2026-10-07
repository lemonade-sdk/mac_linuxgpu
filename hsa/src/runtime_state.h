#pragma once

#include "transport.h"
#include "signal_state.h"
#include <hsa/hsa_ext_amd.h>
#include <hsa/hsa_ven_amd_loader.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <stop_token>
#include <cstdlib>
#include <cstring>
#include <map>

namespace mac_hsa::detail {
struct Agent {
    hsa_agent_t handle;
    std::shared_ptr<Connection> connection; // null for host CPU
};
struct Pool {
    uint64_t handle;
    hsa_agent_t owner;
    size_t capacity;
    std::shared_ptr<Connection> connection;
    bool sharedHost = false; // CPU-owned GTT, single-writer coarse memory and kernargs
};
struct Allocation {
    hsa_amd_pointer_type_t type = HSA_EXT_POINTER_TYPE_HSA;
    hsa_access_permission_t access = HSA_ACCESS_PERMISSION_RW;
    BufferToken ipcToken{};
    uint32_t ipcReferences = 1;
    std::shared_ptr<void> backing; // mappings retain their reservation and storage
    void *base = nullptr;
    size_t size = 0;
    hsa_agent_t owner{};
    void *userData = nullptr;
    uint32_t globalFlags = 0; // zero uses the legacy host/device default
    std::shared_ptr<Connection> connection;
    DeviceBuffer buffer;
    SharedBuffer shared;
    // Device VRAM the CPU was granted (hsa_amd_agents_allow_access with a
    // CPU agent, BAR writes): mapped write combined at its own address.
    bool cpuMapped = false;
    hsa_status_t release() {
        if (!connection || !buffer.handle) return HSA_STATUS_SUCCESS;
        const auto status = shared.host ? connection->freeSharedBuffer(shared) : connection->freeBuffer(buffer);
        if (status == HSA_STATUS_SUCCESS) { buffer = {}; shared = {}; base = nullptr; }
        return status;
    }
    ~Allocation() {
        if (connection) { if (buffer.handle) release(); }
        else if (!backing && type == HSA_EXT_POINTER_TYPE_HSA) std::free(base);
    }
};
// An hsa_amd_memory_async_copy: run by a copy worker (memory.cpp), each
// job on a thread of its own as long as it runs, so a copy that waits for
// its dependencies never holds up another; the threads are kept and reused
// rather than made for every copy. Destroying a job waits for it to finish
// (a stop request makes a job that has not copied yet fail at once).
struct CopyJob {
    std::atomic<bool> done{false};
    std::stop_source stop;
    std::function<void(std::stop_token)> work;
    ~CopyJob();
};
// Queues @job for a copy worker, starting one when every worker is busy.
void startCopyJob(CopyJob *job);

/* The runtime's process-wide state lives until the process ends and is
 * never destroyed: a client may exit with executables, queues and buffers
 * still loaded (without hsa_shut_down), and destroying them from static
 * destructors would free GPU memory through connections and mutexes that
 * other translation units' destructors have already finalized (build 246
 * aborted every LSE run in exit() so). The driver reclaims what a client
 * held when its connection closes with the process. */
extern std::mutex &runtimeMutex;
extern std::recursive_mutex &executableLifecycleMutex;
void clearLoadedImages(); // caller holds runtimeMutex and executableLifecycleMutex
hsa_status_t loaderExtensionTable(size_t size, void *table);
extern uint32_t references;
extern uint64_t lastHandle;
extern std::vector<Agent> &agents;
extern std::unordered_map<uint64_t, std::shared_ptr<Signal>> &signals;
extern std::vector<Pool> &pools;
extern std::map<uintptr_t, std::shared_ptr<Allocation>> &allocations;
extern std::vector<std::unique_ptr<CopyJob>> &copyJobs;
struct Executable;
struct ExecutableSymbol;
struct CodeReader;
extern std::unordered_map<uint64_t, std::shared_ptr<Executable>> &executables;
extern std::unordered_map<uint64_t, std::shared_ptr<ExecutableSymbol>> &executableSymbols;
extern std::unordered_map<uint64_t, std::shared_ptr<CodeReader>> &codeReaders;

// Caller holds runtimeMutex. IDs are never reused across runtime sessions.
Agent *findAgent(hsa_agent_t handle);
Pool *findPool(uint64_t handle);
std::shared_ptr<Allocation> findAllocation(const void *pointer);
std::shared_ptr<Signal> findSignal(hsa_signal_t handle);
struct RuntimeQueue;
using RetiredQueueSet=std::unordered_map<const hsa_queue_t *,std::shared_ptr<RuntimeQueue>>;
RetiredQueueSet clearQueues(); // retire under runtimeMutex; destroy after unlocking
void stopQueueServices(RetiredQueueSet &);
// Runs the device's code-cache synchronization (a one-instruction kernel
// whose SYSTEM-scope acquire fence invalidates the agent's instruction and
// data caches) as an AQL packet on one of this connection's live runtime
// queues. Used when the driver's bounded code-sync launch cannot borrow a
// queue slot because the runtime's queues hold them all. Returns
// HSA_STATUS_ERROR_OUT_OF_RESOURCES, having submitted nothing, when the
// connection has no usable queue.
hsa_status_t codeSyncOnRuntimeQueue(const std::shared_ptr<Connection> &connection);
// The code sync owed for code loaded since the last one (transport.h,
// kCodeSyncDriverBuild), run now if any: before a doorbell.
hsa_status_t flushPendingCodeSync(const std::shared_ptr<Connection> &connection);
size_t hostPageSize();
void clearVirtualMemory(); // caller holds runtimeMutex; allocation pins retain mappings
void clearHostLocks();
bool describeHostLock(const void *pointer, hsa_amd_pointer_info_t &info); // caller holds runtimeMutex
void clearCaches();
void reapCopyJobs();
void clearSystemEvents();
hsa_status_t deliverSystemEvent(const hsa_amd_event_t &event);
hsa_status_t createGPUSignalBacking(const std::shared_ptr<Connection> &, int64_t, const std::shared_ptr<Signal> &);
void invalidateGPUSignals(const std::shared_ptr<Connection> &);
hsa_status_t reclaimGPUSignalService(const std::shared_ptr<Connection> &,std::shared_ptr<void> *lease=nullptr);
hsa_status_t createIPCSignal(hsa_signal_value_t initial, uint32_t count, const hsa_agent_t *consumers, hsa_signal_t *out);
// Device power (power.h, power.cpp). Submissions a client's prepare holds
// back on a connection; the status a failure on @connection reports
// (kDeviceLostStatus once the device lost its memory); and, for the
// connection's runtime queues: wait until every packet the driver was asked
// to run has been read (false at @deadline), ring doorbells that waited,
// and count the queues holding one.
bool submissionsHeld(const Connection *connection);
void holdSubmissions(const Connection *connection, bool hold);
hsa_status_t deviceStatus(const std::shared_ptr<Connection> &connection, hsa_status_t status);
bool drainQueues(const std::shared_ptr<Connection> &connection, std::chrono::steady_clock::time_point deadline);
void replayQueues(const std::shared_ptr<Connection> &connection);
uint32_t pausedQueues(const std::shared_ptr<Connection> &connection);

template<typename T> hsa_status_t writeValue(void *output, T value) {
    std::memcpy(output, &value, sizeof(value));
    return HSA_STATUS_SUCCESS;
}
} // namespace mac_hsa::detail
