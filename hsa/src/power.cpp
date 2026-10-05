// Device power for runtime clients (mac_hsa_power.h, power.h).
#include "runtime_state.h"
#include "mac_hsa_power.h"
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

namespace mac_hsa::detail {
namespace {
std::mutex &heldMutex = *new std::mutex;
std::set<const Connection *> &held = *new std::set<const Connection *>;

std::shared_ptr<Connection> agentConnection(hsa_agent_t agent, hsa_status_t &status) {
    std::lock_guard lock(runtimeMutex);
    status = HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!references) return nullptr;
    status = HSA_STATUS_ERROR_INVALID_AGENT;
    const auto found = findAgent(agent);
    if (!found || !found->connection) return nullptr;
    status = HSA_STATUS_SUCCESS;
    return found->connection;
}

bool fill(const std::shared_ptr<Connection> &connection, const PowerSnapshot &snapshot,
          mac_hsa_power_state_t *out, size_t size) {
    if (!out) return true;
    if (size != sizeof(*out)) return false;
    using namespace amdgpu::power;
    *out = {};
    out->version = 1;
    out->state = uint32_t(snapshot.words[State]);
    out->flags = snapshot.flags();
    out->generation = snapshot.generation();
    out->cause = uint32_t(snapshot.words[Cause]);
    out->error = int32_t(int64_t(snapshot.words[Error]));
    out->holds = uint32_t(snapshot.words[Holds]);
    out->paused_queues = pausedQueues(connection);
    out->quiesces = snapshot.words[Quiesces];
    out->losses = snapshot.words[Losses];
    out->last_transition_us = snapshot.words[LastTransitionMicroseconds];
    return true;
}
} // namespace

bool submissionsHeld(const Connection *connection) {
    std::lock_guard lock(heldMutex);
    return held.contains(connection);
}

void holdSubmissions(const Connection *connection, bool hold) {
    std::lock_guard lock(heldMutex);
    if (hold) held.insert(connection);
    else held.erase(connection);
}

hsa_status_t deviceStatus(const std::shared_ptr<Connection> &connection, hsa_status_t status) {
    PowerSnapshot snapshot;
    if (connection && connection->powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.lost())
        return kDeviceLostStatus;
    return status;
}
} // namespace mac_hsa::detail

using namespace mac_hsa;
using namespace mac_hsa::detail;

extern "C" {
HSA_API_EXPORT hsa_status_t mac_hsa_agent_get_power_state(hsa_agent_t agent, mac_hsa_power_state_t *out,
                                                          size_t size) {
    if (!out || size != sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    hsa_status_t status;
    const auto connection = agentConnection(agent, status);
    if (!connection) return status;
    PowerSnapshot snapshot;
    status = connection->powerState(snapshot);
    if (status != HSA_STATUS_SUCCESS) return status;
    fill(connection, snapshot, out, size);
    return HSA_STATUS_SUCCESS;
}

HSA_API_EXPORT hsa_status_t mac_hsa_agent_prepare_low_power(hsa_agent_t agent, uint32_t drainTimeoutMS,
                                                            mac_hsa_power_state_t *out, size_t size) {
    if (out && size != sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    hsa_status_t status;
    const auto connection = agentConnection(agent, status);
    if (!connection) return status;
    // Stop ringing; let what was rung finish (bounded); then the driver
    // unmaps the queues and saves whatever still runs.
    holdSubmissions(connection.get(), true);
    (void)drainQueues(connection, std::chrono::steady_clock::now() + std::chrono::milliseconds(drainTimeoutMS));
    PowerSnapshot snapshot;
    status = connection->requestPower(amdgpu::power::Prepare, snapshot);
    if (status != HSA_STATUS_SUCCESS) {
        holdSubmissions(connection.get(), false);
        replayQueues(connection);
        return status;
    }
    fill(connection, snapshot, out, size);
    return snapshot.lost() ? kDeviceLostStatus : HSA_STATUS_SUCCESS;
}

HSA_API_EXPORT hsa_status_t mac_hsa_agent_resume(hsa_agent_t agent, mac_hsa_power_state_t *out, size_t size) {
    if (out && size != sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    hsa_status_t status;
    const auto connection = agentConnection(agent, status);
    if (!connection) return status;
    PowerSnapshot snapshot;
    status = connection->requestPower(amdgpu::power::Resume, snapshot);
    holdSubmissions(connection.get(), false);
    if (status != HSA_STATUS_SUCCESS) return status;
    // Ring what waited; a device still suspended for another holder refuses
    // and the queues keep waiting (their service rings them later).
    if (snapshot.takingWork()) replayQueues(connection);
    fill(connection, snapshot, out, size);
    return snapshot.lost() ? kDeviceLostStatus : HSA_STATUS_SUCCESS;
}

HSA_API_EXPORT hsa_status_t mac_hsa_agent_wait_power_state(hsa_agent_t agent, uint64_t known,
                                                           uint32_t timeoutMS, mac_hsa_power_state_t *out,
                                                           size_t size) {
    if (out && size != sizeof(*out)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    hsa_status_t status;
    const auto connection = agentConnection(agent, status);
    if (!connection) return status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMS);
    PowerSnapshot snapshot;
    for (;;) {
        status = connection->powerState(snapshot);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (snapshot.generation() != known || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fill(connection, snapshot, out, size);
    return HSA_STATUS_SUCCESS;
}
} // extern "C"
