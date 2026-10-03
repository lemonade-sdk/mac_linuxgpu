#include "runtime_state.h"
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <set>
#include <thread>
#include <unistd.h>

namespace mac_hsa::detail {
std::mutex runtimeMutex;
uint32_t references = 1;
std::vector<Agent> agents;
std::map<uintptr_t, std::shared_ptr<Allocation>> allocations;
Agent *findAgent(hsa_agent_t handle) {
    for (auto &agent : agents) if (agent.handle.handle == handle.handle) return &agent;
    return nullptr;
}
std::shared_ptr<Allocation> findAllocation(const void *pointer) {
    auto it = allocations.upper_bound(uintptr_t(pointer));
    if (it == allocations.begin()) return {};
    --it;
    return uintptr_t(pointer) - it->first < it->second->size ? it->second : nullptr;
}
}

using namespace mac_hsa;
using namespace mac_hsa::detail;
class MockConnection final : public Connection {
    std::mutex mutex;
    std::condition_variable changed;
    std::set<uint64_t> live;
public:
    unsigned imports = 0, releases = 0;
    bool synchronize = true;
    hsa_status_t read(DeviceSnapshot &) override { return HSA_STATUS_SUCCESS; }
    hsa_status_t importBuffer(const BufferToken &token, DeviceBuffer &buffer) override {
        std::unique_lock lock(mutex);
        const auto handle = ++imports;
        live.insert(handle);
        changed.notify_all();
        if (synchronize) changed.wait(lock, [&] { return imports == 2; });
        buffer = {handle, 0x800000000ull, token.size};
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        // A last-reference release may call arbitrary transport code. Holding
        // the registry mutex here would deadlock a transport reentry.
        std::lock_guard registry(runtimeMutex);
        std::lock_guard lock(mutex);
        assert(live.erase(buffer.handle) == 1);
        ++releases;
        return HSA_STATUS_SUCCESS;
    }
};

int main() {
    alarm(15);
    const hsa_agent_t gpu{17};
    auto connection = std::make_shared<MockConnection>();
    agents.push_back({gpu, connection});
    BufferToken token{5, {23, 29}, 16384};
    hsa_amd_ipc_memory_t handle{};
    std::memcpy(&handle, &token, sizeof(handle));
    void *pointers[2]{};
    hsa_status_t statuses[2]{};
    std::thread first([&] { statuses[0] = hsa_amd_ipc_memory_attach(&handle, token.size, 1, &gpu, &pointers[0]); });
    std::thread second([&] { statuses[1] = hsa_amd_ipc_memory_attach(&handle, token.size, 1, &gpu, &pointers[1]); });
    first.join(); second.join();
    assert(statuses[0] == HSA_STATUS_SUCCESS && statuses[1] == HSA_STATUS_SUCCESS);
    assert(pointers[0] && pointers[0] == pointers[1]);
    assert(connection->imports == 2 && connection->releases == 1);
    assert(allocations.size() == 1 && allocations.begin()->second->ipcReferences == 2);

    void *again = nullptr;
    assert(hsa_amd_ipc_memory_attach(&handle, token.size, 1, &gpu, &again) == HSA_STATUS_SUCCESS);
    assert(again == pointers[0] && connection->imports == 2);
    assert(allocations.begin()->second->ipcReferences == 3);
    connection->synchronize = false;
    ++token.token[0];
    std::memcpy(&handle, &token, sizeof(handle));
    assert(hsa_amd_ipc_memory_attach(&handle, token.size, 1, &gpu, &again) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    assert(!again && connection->imports == 3 && connection->releases == 2);
    assert(allocations.begin()->second->ipcReferences == 3);

    assert(hsa_amd_ipc_memory_detach(pointers[0]) == HSA_STATUS_SUCCESS);
    assert(hsa_amd_ipc_memory_detach(pointers[0]) == HSA_STATUS_SUCCESS);
    assert(connection->releases == 2);
    assert(hsa_amd_ipc_memory_detach(pointers[0]) == HSA_STATUS_SUCCESS);
    assert(allocations.empty() && connection->releases == 3);
    assert(hsa_amd_ipc_memory_detach(pointers[0]) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    agents.clear();
    std::puts("PASS HSA IPC concurrent duplicate imports, balanced detach, alias rejection and unlocked BO release");
}
