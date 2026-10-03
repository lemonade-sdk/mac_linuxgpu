#pragma once
#include "transport.h"
#include <span>
#include <string_view>

namespace mac_hsa {
enum class DriverProtocol { Unknown, Converted, LinuxShim };
constexpr DriverProtocol driverProtocol(std::string_view bundle, std::string_view userClass) {
    if (bundle != "com.geramyloveless.MacAMDGPUHost.MacAMDGPU") return DriverProtocol::Unknown;
    if (userClass == "MacLinuxGPU") return DriverProtocol::LinuxShim;
    if (userClass == "MacAMDGPU") return DriverProtocol::Converted;
    return DriverProtocol::Unknown;
}
struct FirmwareFile { uint64_t type; std::string name; };
class InitializationRPC {
public:
    virtual ~InitializationRPC() = default;
    virtual hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input,
                                std::span<uint64_t> output) = 0;
    virtual hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) = 0;
    virtual hsa_status_t uploadFirmware(const FirmwareFile &file) = 0;
    virtual void waitAfterReset() = 0;
};
// Caller serializes the session. Acquiring identity must succeed before reset;
// a stage cached by another owner is never treated as this session's readiness.
hsa_status_t initializeDevice(InitializationRPC &rpc, bool &claimed, uint64_t &capacity, bool allowInitialize = true);
}

namespace mac_hsa {
class ShimInitializationRPC : public InitializationRPC {
public:
    virtual hsa_status_t reserveHostWindow(uint64_t bytes, uint64_t &base) = 0;
    virtual hsa_status_t releaseHostWindow(uint64_t base, uint64_t bytes) = 0;
    // On-demand firmware. Upstream calls request_firmware() from inside
    // InitDevice (selector 9), which runs on the driver's serial queue, so the
    // driver reads files through a shared-memory mailbox that this client
    // must service while the call is in flight. initializeShimDevice brackets
    // selector 9 with these. startFirmwareService returns false when no
    // servicer could run (initialization then continues and the driver falls
    // back to its embedded firmware only); stopFirmwareService is called
    // exactly once after every start that returned true, on every exit path.
    // The converted protocol never calls them: it uploads its own firmware.
    virtual bool startFirmwareService() = 0;
    virtual void stopFirmwareService() = 0;
};
struct ShimInitializationResult {
    uint64_t capacity=0, hostBase=0, hostBytes=0;
    ComputeSessionMode sessionMode=ComputeSessionMode::Unknown;
};
enum class ShimInitializationOperation { None, Scalar, Validation, ReserveHostWindow, ReleaseHostWindow };
struct ShimInitializationFailure {
    ShimInitializationOperation operation = ShimInitializationOperation::None;
    uint32_t selector = UINT32_MAX;
    uint64_t tag = 0;
    bool hasTag = false;
    hsa_status_t status = HSA_STATUS_SUCCESS;
};
hsa_status_t initializeShimDevice(ShimInitializationRPC &, ShimInitializationResult &,
                                  bool &claimed, bool allowInitialize = true,
                                  ShimInitializationFailure *failure = nullptr);
}
