#include "device_init.h"
#include "isa_target.h"
#include <array>
#include <cstdio>

namespace mac_hsa {
namespace {
constexpr uint64_t kRuntimeMagic = 0x414d444750554142ull;
constexpr uint64_t kAMDVendorID = 0x1002;

// Upstream amdgpu firmware naming (amdgpu_ucode_ip_version_decode): an IP
// block's file prefix is "<ip>_<major>_<minor>_<revision>". IP versions arrive
// packed as major<<16 | minor<<8 | revision.
std::string ipPrefix(const char *ip, uint64_t packed) {
    char name[48];
    std::snprintf(name, sizeof(name), "%s_%u_%u_%u", ip, unsigned((packed >> 16) & 255),
                  unsigned((packed >> 8) & 255), unsigned(packed & 255));
    return name;
}
bool validPackedIP(uint64_t packed) { return packed && packed <= 0xffffff && (packed >> 16); }

// amdgpu_ucode.c kicker_device_list: boards whose PSP/SMU/IMU/RLC images are
// the "_kicker" variants. Mirrors upstream's table verbatim; the Linux-shim
// driver resolves this itself, only the converted protocol needs it here.
struct KickerDevice { uint16_t device; uint8_t revision; };
constexpr KickerDevice kKickerDevices[] = {{0x744B, 0x00}, {0x7551, 0xC8}};
bool kickerFirmware(uint64_t device, uint64_t revision) {
    for (const auto &entry : kKickerDevices)
        if (entry.device == device && entry.revision == revision) return true;
    return false;
}

// Converted-protocol firmware set, named from the IP versions the device
// reported at discovery. The protocol's load sequence (PSP SOS/TA, SMU, SDMA,
// then the GFX12 RLC-autoload set PFP/ME/MEC/unified MES/IMU/RLC, with the
// converted driver's PSP ucode type IDs) is defined only for GC major 12.
bool convertedFirmware(uint64_t gcMajor, uint64_t gcMinor, uint64_t gcRevision,
                       const std::array<uint64_t, 4> &ips, bool kicker,
                       std::vector<FirmwareFile> &out) {
    if (gcMajor != 12 || !validPackedIP(ips[1]) || !validPackedIP(ips[2]) || !validPackedIP(ips[3]))
        return false;
    const std::string suffix = kicker ? "_kicker.bin" : ".bin";
    const auto gc = ipPrefix("gc", (gcMajor << 16) | (gcMinor << 8) | gcRevision);
    const auto psp = ipPrefix("psp", ips[2]), smu = ipPrefix("smu", ips[3]);
    out = {{0, psp + "_sos" + suffix}, {9, psp + "_ta" + suffix},
           {274, smu + suffix}, {512, ipPrefix("sdma", ips[1]) + ".bin"},
           {516, gc + "_pfp.bin"}, {517, gc + "_me.bin"},
           {518, gc + "_mec.bin"}, {515, gc + "_uni_mes.bin"},
           {514, gc + "_imu" + suffix}, {513, gc + "_rlc" + suffix}};
    return true;
}

// GART host window: the driver reports its fixed aperture size, which must be
// a power of two (the window is size-aligned) that fits below the 47-bit
// user VA limit with the 4 GiB floor the driver enforces.
bool validWindowSize(uint64_t bytes) {
    return bytes >= 16384 && !(bytes & (bytes - 1)) && bytes <= (1ull << 45);
}
bool validWindow(uint64_t base, uint64_t bytes) {
    return validWindowSize(bytes) && base >= (1ull << 32) && !(base & (bytes - 1)) &&
        base < (1ull << 47) - bytes;
}

// Runs the client's firmware servicer for the lifetime of the scope (see
// ShimInitializationRPC::startFirmwareService). Stops it on every exit.
class FirmwareServiceScope {
public:
    explicit FirmwareServiceScope(ShimInitializationRPC &rpc)
        : rpc_(rpc), running_(rpc.startFirmwareService()) {}
    ~FirmwareServiceScope() { if (running_) rpc_.stopFirmwareService(); }
    FirmwareServiceScope(const FirmwareServiceScope &) = delete;
    FirmwareServiceScope &operator=(const FirmwareServiceScope &) = delete;
private:
    ShimInitializationRPC &rpc_;
    const bool running_;
};
}

hsa_status_t initializeDevice(InitializationRPC &rpc, bool &claimed, uint64_t &capacity, bool allowInitialize) {
    claimed = false; capacity = 0;
    std::array<uint64_t, 3> build{};
    auto status = rpc.scalar(43, {}, build);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (build[0] != kRuntimeMagic || build[1] != 1 || build[2] < 179)
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    std::array<uint64_t, 7> identity{};
    status = rpc.scalar(1, {}, identity);
    if (status != HSA_STATUS_SUCCESS) return status;
    claimed = true;
    if (identity[3] != kAMDVendorID || !identity[4] || identity[4] > UINT16_MAX)
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    uint64_t tag = 4, stage = UINT64_MAX;
    status = rpc.scalar(21, {&tag, 1}, {&stage, 1});
    if (status != HSA_STATUS_SUCCESS) return status;
    const auto discovered = [&](std::array<uint64_t, 4> &ips, std::array<uint64_t, 3> &gfx) {
        tag = 3;
        auto result = rpc.scalar(21, {&tag, 1}, ips);
        if (result != HSA_STATUS_SUCCESS) return result;
        tag = 1;
        result = rpc.scalar(21, {&tag, 1}, gfx);
        if (result != HSA_STATUS_SUCCESS) return result;
        if (!gfx[0] || gfx[0] > 255 || gfx[1] > 255 || gfx[2] > 255 ||
            !gfxTargetVersionFromGCVersion(uint32_t(gfx[0]), uint32_t(gfx[1]), uint32_t(gfx[2])))
            return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
        return HSA_STATUS_SUCCESS;
    };
    const auto accounted = [&] {
        tag = 5;
        std::array<uint64_t, 15> accounting{};
        auto result = rpc.scalar(21, {&tag, 1}, accounting);
        if (result != HSA_STATUS_SUCCESS) return result;
        if (accounting[0] != 1 || !(accounting[1] & 1) || !accounting[10]) return HSA_STATUS_ERROR;
        capacity = accounting[10];
        return HSA_STATUS_SUCCESS;
    };
    std::array<uint64_t, 4> ips{};
    std::array<uint64_t, 3> gfx{};
    if (stage == 15 && build[2] >= 180) {
        // Build 180 attaches this client to the driver-owned ready session in
        // GetIdentity. Older drivers only permit the original owning client.
        // Never reset or reload firmware when joining an initialized peer.
        status = discovered(ips, gfx);
        if (status != HSA_STATUS_SUCCESS) return status;
        return accounted();
    }
    if (!allowInitialize || stage != 0) return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;

    const std::array<uint64_t, 2> dmaInput{32ull << 20, 16384};
    std::array<uint64_t, 2> dmaOutput{};
    status = rpc.scalar(6, dmaInput, dmaOutput);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!dmaOutput[0]) return HSA_STATUS_ERROR;
    status = rpc.scalar(8, {}, {});
    if (status != HSA_STATUS_SUCCESS) return status;
    rpc.waitAfterReset();
    const auto advance = [&](uint64_t target) {
        uint64_t reached = 0;
        auto result = rpc.scalar(9, {&target, 1}, {&reached, 1});
        if (result != HSA_STATUS_SUCCESS) return result;
        return reached == target ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    };
    status = advance(1); // IFWI wait + discovery, bounded in the driver
    if (status != HSA_STATUS_SUCCESS) return status;
    status = discovered(ips, gfx);
    if (status != HSA_STATUS_SUCCESS) return status;
    // Firmware names follow from the discovered IP versions, so they resolve
    // after discovery but before stage 2 loads anything: a missing file still
    // fails with no firmware handed to the PSP.
    std::vector<FirmwareFile> firmware;
    if (!convertedFirmware(gfx[0], gfx[1], gfx[2], ips, kickerFirmware(identity[4], identity[6]), firmware))
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    status = rpc.prepareFirmware(firmware);
    if (status != HSA_STATUS_SUCCESS) return status;
    for (uint64_t target = 2; target <= 4; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    status = rpc.uploadFirmware(firmware.front());
    if (status != HSA_STATUS_SUCCESS) return status;
    for (uint64_t target = 5; target <= 7; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    for (size_t i = 1; i < firmware.size(); ++i) {
        status = rpc.uploadFirmware(firmware[i]);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    // Each firmware RPC waits for its PSP result/fence, and stage 8 validates
    // the completed load set. No arbitrary sleep substitutes for those acks.
    for (uint64_t target = 8; target <= 15; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    return accounted();
}

hsa_status_t initializeShimDevice(ShimInitializationRPC &rpc,
    ShimInitializationResult &out, bool &claimed, bool allowInitialize,
    ShimInitializationFailure *failure) {
    out={}; claimed=false;
    if (failure) *failure = {};
    uint32_t currentSelector = UINT32_MAX;
    uint64_t currentTag = 0;
    bool currentHasTag = false;
    const auto failed = [&](hsa_status_t status, ShimInitializationOperation operation) {
        if (failure && failure->operation == ShimInitializationOperation::None)
            *failure = {operation, currentSelector, currentTag, currentHasTag, status};
        return status;
    };
    const auto scalar = [&](uint32_t selector, std::span<const uint64_t> input,
                            std::span<uint64_t> output) {
        currentSelector = selector;
        currentHasTag = selector == 21 && !input.empty();
        currentTag = currentHasTag ? input[0] : 0;
        const auto status = rpc.scalar(selector, input, output);
        if (status != HSA_STATUS_SUCCESS) failed(status, ShimInitializationOperation::Scalar);
        return status;
    };
    const auto invalid = [&](hsa_status_t status) {
        return failed(status, ShimInitializationOperation::Validation);
    };
    std::array<uint64_t,3> build{};
    auto status=scalar(43,{},build);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (build[0]!=kRuntimeMagic || build[1]!=1)
        return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    // Claim the sole compute session before changing its host window. A
    // competing client must fail acquisition without reconfiguring the GPU.
    std::array<uint64_t,7> identity{};
    status=scalar(1,{},identity);
    if (status!=HSA_STATUS_SUCCESS) return status;
    claimed=true;
    // Any AMD GPU the driver bound; the ISA is resolved from what it reports.
    if (identity[3]!=kAMDVendorID || !identity[4] || identity[4]>UINT16_MAX)
        return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    uint64_t tag=4, stage=0;
    status=scalar(21,{&tag,1},{&stage,1});
    if (status!=HSA_STATUS_SUCCESS) return status;
    uint64_t requested=0, windowBytes=0;
    std::array<uint64_t,3> window{};
    uint64_t query=0;
    if (stage!=2) {
        if (!allowInitialize || stage!=0)
            return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
        // Before probe the driver answers a query with the fixed GART
        // aperture size it will map (and the base requested so far, if
        // any); reserve exactly that size.
        status=scalar(54,{&query,1},window);
        if (status!=HSA_STATUS_SUCCESS) return status;
        windowBytes=window[1];
        if (!validWindowSize(windowBytes))
            return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
        status=rpc.reserveHostWindow(windowBytes,requested);
        if (status!=HSA_STATUS_SUCCESS) {
            currentSelector = UINT32_MAX; currentHasTag = false;
            return failed(status, ShimInitializationOperation::ReserveHostWindow);
        }
        if (!validWindow(requested,windowBytes)) {
            currentSelector = UINT32_MAX; currentHasTag = false;
            failed(HSA_STATUS_ERROR_OUT_OF_RESOURCES, ShimInitializationOperation::ReserveHostWindow);
            (void)rpc.releaseHostWindow(requested,windowBytes);
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        std::array<uint64_t,3> configured{};
        status=scalar(54,{&requested,1},configured);
        if (status==HSA_STATUS_SUCCESS &&
            (configured[0]!=requested || configured[1]!=windowBytes))
            status=invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
        if (status==HSA_STATUS_SUCCESS) {
            // Upstream requests firmware inside the probe; serve it from disk
            // while InitDevice is in flight.
            FirmwareServiceScope firmware(rpc);
            status=scalar(9,{},{});
        }
        const auto release=rpc.releaseHostWindow(requested,windowBytes);
        if (release!=HSA_STATUS_SUCCESS) {
            currentSelector = UINT32_MAX; currentHasTag = false;
            return failed(release, ShimInitializationOperation::ReleaseHostWindow);
        }
        if (status!=HSA_STATUS_SUCCESS) return status;
    }
    // A ready session retains its existing window; never reconfigure it.
    status=scalar(43,{},build);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (build[0]!=kRuntimeMagic || build[1]!=1 ||
        build[2]<kQueueResourceDriverBuild)
        return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    status=scalar(21,{&tag,1},{&stage,1});
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (stage!=2) return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    // Which compute session the driver gives this client (tag 12). Asking
    // is what opts in: a KFD-backed client then owns a host window in its
    // own GPUVM, reserved and set here like the pre-probe GART window. A
    // driver that predates the tag keeps the GART window as host window.
    ComputeSessionMode sessionMode=ComputeSessionMode::Unknown;
    if (build[2]>=kComputeSessionDriverBuild) {
        std::array<uint64_t,kComputeSessionQueryWords> session{};
        const uint64_t sessionTag=kComputeSessionQueryTag;
        if (rpc.scalar(21,{&sessionTag,1},session)==HSA_STATUS_SUCCESS && session[SessionVersion]>=1 &&
            (session[SessionMode]==uint64_t(ComputeSessionMode::Legacy) ||
             session[SessionMode]==uint64_t(ComputeSessionMode::KFD)))
            sessionMode=ComputeSessionMode(session[SessionMode]);
    }
    query=0;
    status=scalar(54,{&query,1},window);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (sessionMode==ComputeSessionMode::KFD) {
        if (!window[0]) {
            const uint64_t bytes=window[1];
            uint64_t candidate=0;
            if (!validWindowSize(bytes))
                return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
            status=rpc.reserveHostWindow(bytes,candidate);
            if (status!=HSA_STATUS_SUCCESS) {
                currentSelector = UINT32_MAX; currentHasTag = false;
                return failed(status, ShimInitializationOperation::ReserveHostWindow);
            }
            if (!validWindow(candidate,bytes)) {
                currentSelector = UINT32_MAX; currentHasTag = false;
                failed(HSA_STATUS_ERROR_OUT_OF_RESOURCES, ShimInitializationOperation::ReserveHostWindow);
                (void)rpc.releaseHostWindow(candidate,bytes);
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            }
            std::array<uint64_t,3> configured{};
            status=scalar(54,{&candidate,1},configured);
            // Only a placement hint, as for the GART window: every shared
            // buffer is later mapped at its own GPU VA.
            const auto release=rpc.releaseHostWindow(candidate,bytes);
            if (release!=HSA_STATUS_SUCCESS) {
                currentSelector = UINT32_MAX; currentHasTag = false;
                return failed(release, ShimInitializationOperation::ReleaseHostWindow);
            }
            if (status!=HSA_STATUS_SUCCESS) return status;
            if (configured[0]!=candidate || configured[1]!=bytes)
                return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
            window=configured;
        }
        if (!validWindow(window[0],window[1]) || window[2]!=1)
            return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    } else if (!validWindow(window[0],window[1]) || window[2]!=1 ||
        (windowBytes && window[1]!=windowBytes) || (requested && window[0]!=requested))
        return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    std::array<uint64_t,2> vram{}; tag=2;
    status=scalar(21,{&tag,1},vram);
    if (status!=HSA_STATUS_SUCCESS) return status;
    std::array<uint64_t,6> usage{}; tag=9;
    status=scalar(21,{&tag,1},usage);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (!vram[0] || vram[0]>vram[1] || usage[0]!=vram[1] || !usage[1] ||
        usage[1]>usage[0] || usage[2]>usage[0] || usage[3]>usage[1] ||
        usage[4]!=vram[0] || usage[5]>usage[4])
        return invalid(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    out={usage[1],window[0],window[1],sessionMode};
    return HSA_STATUS_SUCCESS;
}
}
