#include "runtime_state.h"
#include <array>

using namespace mac_hsa::detail;
namespace {
// A GPU agent exposes two ISAs, as ROCr does: its own processor and, when
// LLVM defines one, the generic family it belongs to. The ISA handle is the
// agent handle, with this bit set for the generic family.
constexpr uint64_t kGenericIsaBit = 1ull << 62;

hsa_status_t isaTarget(uint64_t handle, mac_hsa::IsaTarget &target, bool &generic) {
    generic = handle & kGenericIsaBit;
    std::shared_ptr<mac_hsa::Connection> connection;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto agent = findAgent({handle & ~kGenericIsaBit});
        if (!agent || !agent->connection) return HSA_STATUS_ERROR_INVALID_ISA;
        connection = agent->connection;
    }
    mac_hsa::DeviceSnapshot snapshot;
    const auto status = connection->read(snapshot);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!mac_hsa::deviceIsa(snapshot, target) || (generic && target.generic.empty()))
        return HSA_STATUS_ERROR_INVALID_ISA;
    return HSA_STATUS_SUCCESS;
}
}
extern "C" {
hsa_status_t hsa_agent_iterate_isas(hsa_agent_t handle,
    hsa_status_t (*callback)(hsa_isa_t, void *), void *data) {
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto agent = findAgent(handle);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!agent->connection) return HSA_STATUS_SUCCESS; // no CPU kernel agent
        if (handle.handle & kGenericIsaBit) return HSA_STATUS_ERROR_INVALID_AGENT;
    }
    mac_hsa::IsaTarget target;
    bool generic = false;
    const auto status = isaTarget(handle.handle, target, generic);
    if (status != HSA_STATUS_SUCCESS) return status;
    // The specific processor first: the most capable ISA for this agent.
    const auto result = callback({handle.handle}, data);
    if (result != HSA_STATUS_SUCCESS || target.generic.empty()) return result;
    return callback({handle.handle | kGenericIsaBit}, data);
}
hsa_status_t hsa_isa_get_info_alt(hsa_isa_t isa, hsa_isa_info_t attribute, void *value) {
    mac_hsa::IsaTarget target;
    bool generic = false;
    const auto status = isaTarget(isa.handle, target, generic);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto name = generic ? target.genericIsaName() : target.isaName();
    const auto limits = mac_hsa::kDispatchLimits;
    switch (attribute) {
    // Match the ROCr ABI used by HRX: NAME_LENGTH includes the NUL byte,
    // despite the older header prose describing the length without it.
    case HSA_ISA_INFO_NAME_LENGTH: return writeValue(value, uint32_t(name.size() + 1));
    case HSA_ISA_INFO_NAME: std::memcpy(value, name.c_str(), name.size() + 1); return HSA_STATUS_SUCCESS;
    case HSA_ISA_INFO_MACHINE_MODELS: return writeValue(value, std::array<bool, 2>{false, true});
    case HSA_ISA_INFO_PROFILES: return writeValue(value, std::array<bool, 2>{true, false});
    case HSA_ISA_INFO_DEFAULT_FLOAT_ROUNDING_MODES:
    case HSA_ISA_INFO_BASE_PROFILE_DEFAULT_FLOAT_ROUNDING_MODES:
        return writeValue(value, std::array<bool, 3>{false, false, true});
    case HSA_ISA_INFO_FAST_F16_OPERATION: return writeValue(value, true);
    case HSA_ISA_INFO_WORKGROUP_MAX_DIM: return writeValue(value, limits.workgroupMaxDim);
    case HSA_ISA_INFO_WORKGROUP_MAX_SIZE: return writeValue(value, limits.workgroupMaxSize);
    case HSA_ISA_INFO_GRID_MAX_DIM: return writeValue(value, limits.gridMaxDim);
    case HSA_ISA_INFO_GRID_MAX_SIZE: return writeValue(value, limits.gridMaxSize);
    case HSA_ISA_INFO_FBARRIER_MAX_SIZE: return writeValue(value, limits.fbarrierMaxSize);
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
}
