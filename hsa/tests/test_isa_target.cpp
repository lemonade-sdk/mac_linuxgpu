// Host unit test: ISA identity, generic-target compatibility, code-object
// acceptance/rejection and signal-kernel selection, for many GPU generations.
// Compiled together with the runtime's isa_target/code_object/signal_kernels
// sources (internal, hidden-visibility code in the dylib).

#include "code_object.h"
#include "device_init.h"
#include "isa_target.h"
#include "signal_kernels.h"
#include "signal_kernels_code.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace mac_hsa;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", std::string(msg).c_str()); } \
} while (0)

static IsaTarget agentFor(uint32_t major, uint32_t minor, uint32_t revision,
                          TargetFeature sramecc = TargetFeature::Any) {
    IsaTarget target;
    const auto version = gfxTargetVersionFromGCVersion(major, minor, revision);
    if (!resolveIsaTarget(version, TargetFeature::Off, sramecc, target)) target = {};
    return target;
}
static std::span<const uint8_t> bundled(const char *target, bool mailbox = false) {
    for (const auto &entry : signal_kernels::kCodeObjects)
        if (!std::strcmp(entry.target, target)) return mailbox ? entry.mailbox : entry.operations;
    return {};
}
static bool loads(std::span<const uint8_t> object, const IsaTarget &agent) {
    CodeObject parsed;
    return !object.empty() && parseCodeObject(object, parsed, agent) && parsed.kernels.size() == 1;
}
static std::vector<uint8_t> withFlags(std::span<const uint8_t> object, uint32_t flags, int abi = -1) {
    std::vector<uint8_t> copy(object.begin(), object.end());
    std::memcpy(copy.data() + 48, &flags, 4);
    if (abi >= 0) copy[8] = uint8_t(abi);
    return copy;
}

int main() {
    // 1. GC IP version -> processor, generic family and ISA names (KFD + LLVM).
    struct Case { uint32_t major, minor, revision; const char *processor, *isa, *generic; uint32_t wave; };
    const Case cases[] = {
        {9, 0, 1, "gfx900", "amdgcn-amd-amdhsa--gfx900:xnack-", "amdgcn-amd-amdhsa--gfx9-generic:xnack-", 64},
        {9, 4, 2, "gfx90a", "amdgcn-amd-amdhsa--gfx90a:xnack-", "", 64},
        {9, 4, 3, "gfx942", "amdgcn-amd-amdhsa--gfx942:xnack-", "amdgcn-amd-amdhsa--gfx9-4-generic:xnack-", 64},
        {10, 1, 10, "gfx1010", "amdgcn-amd-amdhsa--gfx1010:xnack-", "amdgcn-amd-amdhsa--gfx10-1-generic:xnack-", 32},
        {10, 3, 0, "gfx1030", "amdgcn-amd-amdhsa--gfx1030", "amdgcn-amd-amdhsa--gfx10-3-generic", 32},
        {11, 0, 0, "gfx1100", "amdgcn-amd-amdhsa--gfx1100", "amdgcn-amd-amdhsa--gfx11-generic", 32},
        {11, 0, 3, "gfx1101", "amdgcn-amd-amdhsa--gfx1101", "amdgcn-amd-amdhsa--gfx11-generic", 32},
        {11, 5, 1, "gfx1151", "amdgcn-amd-amdhsa--gfx1151", "amdgcn-amd-amdhsa--gfx11-generic", 32},
        {12, 0, 0, "gfx1200", "amdgcn-amd-amdhsa--gfx1200", "amdgcn-amd-amdhsa--gfx12-generic", 32},
        {12, 0, 1, "gfx1201", "amdgcn-amd-amdhsa--gfx1201", "amdgcn-amd-amdhsa--gfx12-generic", 32},
    };
    for (const auto &c : cases) {
        const auto agent = agentFor(c.major, c.minor, c.revision);
        char label[64]; std::snprintf(label, sizeof(label), "GC %u.%u.%u", c.major, c.minor, c.revision);
        CHECK(agent.processor == c.processor, std::string(label) + " -> " + c.processor);
        CHECK(agent.isaName() == c.isa, std::string(label) + " ISA " + c.isa);
        CHECK(agent.genericIsaName() == c.generic, std::string(label) + " generic ISA '" + c.generic + "'");
        CHECK(agent.defaultWavefrontSize == c.wave, std::string(label) + " default wave size");
    }
    IsaTarget unknown;
    CHECK(!gfxTargetVersionFromGCVersion(13, 0, 0) && !resolveIsaTarget(130000, TargetFeature::Off,
          TargetFeature::Any, unknown), "an unmapped GC IP version resolves to no ISA");
    CHECK(processorName(90010) == "gfx90a" && processorName(90012) == "gfx90c" &&
          processorName(120500) == "gfx1250", "gfx_target_version -> name uses hex minor/stepping");
    {
        IsaTarget sramecc;
        CHECK(resolveIsaTarget(90010, TargetFeature::Off, TargetFeature::On, sramecc) &&
              sramecc.isaName() == "amdgcn-amd-amdhsa--gfx90a:sramecc+:xnack-",
              "device-reported sramecc+ appears in the gfx90a ISA name");
        IsaTarget gfx11;
        CHECK(resolveIsaTarget(110000, TargetFeature::On, TargetFeature::On, gfx11) &&
              gfx11.xnack == TargetFeature::Unsupported && gfx11.sramecc == TargetFeature::Unsupported,
              "features a processor lacks are clamped to unsupported");
    }
    CHECK(endProgramEncoding(agentFor(9, 4, 2)) == 0xbf810000u && endProgramEncoding(agentFor(10, 3, 0)) == 0xbf810000u &&
          endProgramEncoding(agentFor(11, 0, 0)) == 0xbfb00000u && endProgramEncoding(agentFor(12, 0, 1)) == 0xbfb00000u,
          "s_endpgm encoding per generation");
    CHECK(hasRsrc1ClampAndIEEE(agentFor(11, 0, 0)) && !hasRsrc1ClampAndIEEE(agentFor(12, 0, 0)),
          "RSRC1 DX10_CLAMP/IEEE_MODE exist through GFX11 only");

    // 2. Generic-target compatibility from e_flags alone.
    const auto gfx12Agent = agentFor(12, 0, 1), gfx90a = agentFor(9, 4, 2);
    CHECK(codeObjectTargetCompatible(0x04e, 4, gfx12Agent), "own processor, code object v6");
    CHECK(codeObjectTargetCompatible(0x04e, 3, gfx12Agent), "own processor, code object v5");
    CHECK(codeObjectTargetCompatible(0x01000059, 4, gfx12Agent), "gfx12-generic v1 on gfx1201");
    CHECK(codeObjectTargetCompatible(0x02000059, 4, gfx12Agent), "a newer generic version is accepted");
    CHECK(!codeObjectTargetCompatible(0x00000059, 4, gfx12Agent), "generic version 0 is rejected");
    CHECK(!codeObjectTargetCompatible(0x01000059, 3, gfx12Agent), "generic object below code object v6 is rejected");
    CHECK(!codeObjectTargetCompatible(0x0100004e, 4, gfx12Agent), "a specific processor never carries a generic version");
    CHECK(!codeObjectTargetCompatible(0x048, 4, gfx12Agent), "gfx1200 object is not gfx1201 code");
    CHECK(!codeObjectTargetCompatible(0x01000054, 4, gfx12Agent), "gfx11-generic is not gfx12 code");
    CHECK(codeObjectTargetCompatible(0x23f, 4, gfx90a), "gfx90a xnack- object on an xnack-off agent");
    CHECK(!codeObjectTargetCompatible(0x33f, 4, gfx90a), "gfx90a xnack+ object on an xnack-off agent");
    CHECK(codeObjectTargetCompatible(0x53f, 4, gfx90a), "gfx90a xnack/sramecc 'any' object");
    CHECK(!codeObjectTargetCompatible(0xc3f, 4, gfx90a), "sramecc+ object when the agent's mode is unknown");
    CHECK(!codeObjectTargetCompatible(0x24e, 4, gfx12Agent), "xnack- request on a processor without xnack");
    CHECK(!codeObjectTargetCompatible(0x01000051, 4, gfx90a), "gfx9-generic excludes gfx90a");
    CHECK(codeObjectTargetCompatible(0x01000151, 4, agentFor(9, 4, 0)), "gfx9-generic includes gfx906");
    CHECK(!codeObjectTargetCompatible(0x01000151, 4, agentFor(9, 4, 3)), "gfx9-generic excludes gfx942");
    CHECK(codeObjectTargetCompatible(0x0100055f, 4, agentFor(9, 5, 0)), "gfx9-4-generic includes gfx950");
    std::string name;
    CHECK(codeObjectTargetName(0xc3f | 0x200, 4, name) && name == "amdgcn-amd-amdhsa--gfx90a:sramecc+:xnack-",
          "target ID string from e_flags");

    // 3. Real code objects: acceptance by target, rejection otherwise.
    struct Load { const char *object; uint32_t major, minor, revision; bool accepted; };
    const Load loadCases[] = {
        {"gfx12-generic", 12, 0, 1, true}, {"gfx12-generic", 12, 0, 0, true},
        {"gfx12-generic", 11, 0, 0, false}, {"gfx12-generic", 10, 3, 0, false},
        {"gfx11-generic", 11, 0, 0, true}, {"gfx11-generic", 11, 5, 1, true},
        {"gfx11-generic", 12, 0, 1, false}, {"gfx10-3-generic", 10, 3, 0, true},
        {"gfx10-3-generic", 10, 3, 6, true}, {"gfx10-3-generic", 10, 1, 10, false},
        {"gfx10-1-generic", 10, 1, 10, true}, {"gfx9-generic", 9, 0, 1, true},
        {"gfx9-generic", 9, 4, 0, true}, {"gfx9-generic", 9, 4, 2, false},
        {"gfx9-4-generic", 9, 4, 3, true}, {"gfx9-4-generic", 9, 4, 2, false},
        {"gfx90a", 9, 4, 2, true}, {"gfx90a", 9, 4, 1, false}, {"gfx908", 9, 4, 1, true},
        {"gfx1250", 12, 1, 0, true}, {"gfx1250", 12, 0, 1, false},
    };
    for (const auto &c : loadCases) {
        char label[96];
        std::snprintf(label, sizeof(label), "%s object %s on GC %u.%u.%u", c.object,
                      c.accepted ? "loads" : "is rejected", c.major, c.minor, c.revision);
        const auto agent = agentFor(c.major, c.minor, c.revision);
        CHECK(loads(bundled(c.object), agent) == c.accepted && loads(bundled(c.object, true), agent) == c.accepted, label);
    }
    {
        const auto generic12 = bundled("gfx12-generic");
        // Metadata must name the target the e_flags describe.
        CHECK(!loads(withFlags(generic12, 0x04e), gfx12Agent), "gfx1201 e_flags with gfx12-generic metadata is rejected");
        CHECK(!loads(withFlags(generic12, 0x01000059, 3), gfx12Agent), "generic object marked code object v5 is rejected");
        CHECK(!loads(withFlags(generic12, 0x00000059), gfx12Agent), "generic object without a generic version is rejected");
        CodeObject parsed;
        CHECK(parseCodeObject(generic12, parsed, gfx12Agent) && parsed.kernels[0].properties == 0x408,
              "gfx12 signal kernel is wave32 with a kernarg pointer");
        CHECK(parseCodeObject(bundled("gfx9-generic"), parsed, agentFor(9, 0, 1)) &&
              parsed.kernels[0].properties == 0x009 && signalKernelProperties(parsed.kernels[0].properties),
              "gfx9 signal kernel (wave64, private segment buffer SGPRs) passes the AQL property check");
        CHECK(parseCodeObject(bundled("gfx10-3-generic"), parsed, agentFor(10, 3, 0)) &&
              parsed.kernels[0].properties == 0x409 && signalKernelProperties(parsed.kernels[0].properties),
              "gfx10.3 signal kernel (wave32, private segment buffer SGPRs) passes the AQL property check");
        CHECK(!signalKernelProperties(0x408 | 0x2) && !signalKernelProperties(0x400),
              "other implicit SGPR inputs, or no kernarg pointer, are refused");
    }

    // 4. Signal-kernel selection: own processor first, then generic family.
    const std::map<std::vector<uint32_t>, std::string> expected = {
        {{12, 0, 1}, "gfx12-generic"}, {{12, 0, 0}, "gfx12-generic"}, {{11, 0, 0}, "gfx11-generic"},
        {{11, 5, 3}, "gfx11-generic"}, {{10, 3, 0}, "gfx10-3-generic"}, {{10, 1, 10}, "gfx10-1-generic"},
        {{9, 4, 2}, "gfx90a"}, {{9, 4, 1}, "gfx908"}, {{9, 4, 3}, "gfx9-4-generic"},
        {{9, 0, 1}, "gfx9-generic"}, {{12, 1, 0}, "gfx1250"},
    };
    for (const auto &[ip, target] : expected) {
        SignalKernelObjects selected;
        const auto agent = agentFor(ip[0], ip[1], ip[2]);
        CHECK(selectSignalKernels(agent, selected) && selected.target == target &&
              loads(selected.operations, agent) && loads(selected.mailbox, agent),
              agent.processor + " selects " + target);
    }
    {
        IsaTarget lonely; // a KFD-mapped processor with no generic family and no bundled object
        SignalKernelObjects selected; std::string error;
        CHECK(resolveIsaTarget(110504, TargetFeature::Off, TargetFeature::Any, lonely) &&
              !selectSignalKernels(lonely, selected, &error) &&
              error.find("gfx1154") != std::string::npos && error.find("build-signal-kernels.sh") != std::string::npos,
              "no compatible signal kernel fails with a clear error: " + error);
    }

    // 5. Converted-protocol firmware names follow the reported IP versions.
    struct ConvertedRPC final : InitializationRPC {
        uint64_t gc[3]; std::array<uint64_t, 4> ips; uint64_t stage = 0;
        std::vector<FirmwareFile> prepared;
        hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> in, std::span<uint64_t> out) override {
            switch (selector) {
            case 43: out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 190; break;
            case 1: out[3] = 0x1002; out[4] = 0x1234; out[6] = 0x01; break;
            case 21:
                if (in[0] == 4) out[0] = stage;
                else if (in[0] == 3) std::copy(ips.begin(), ips.end(), out.begin());
                else if (in[0] == 1) std::copy(gc, gc + 3, out.begin());
                else if (in[0] == 5) { out[0] = 1; out[1] = 1; out[10] = 8ull << 30; }
                break;
            case 6: out[0] = 1; break;
            case 9: out[0] = in[0]; stage = in[0]; break;
            default: break;
            }
            return HSA_STATUS_SUCCESS;
        }
        hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) override { prepared = files; return HSA_STATUS_SUCCESS; }
        hsa_status_t uploadFirmware(const FirmwareFile &) override { return HSA_STATUS_SUCCESS; }
        void waitAfterReset() override {}
    };
    {
        ConvertedRPC rpc; rpc.gc[0] = 12; rpc.gc[1] = 0; rpc.gc[2] = 0;
        rpc.ips = {0x0c0000, 0x070000, 0x0e0002, 0x0e0002};
        bool claimed = false; uint64_t capacity = 0;
        const auto status = initializeDevice(rpc, claimed, capacity);
        std::vector<std::string> names;
        for (const auto &file : rpc.prepared) names.push_back(file.name);
        const std::vector<std::string> want = {"psp_14_0_2_sos.bin", "psp_14_0_2_ta.bin", "smu_14_0_2.bin",
            "sdma_7_0_0.bin", "gc_12_0_0_pfp.bin", "gc_12_0_0_me.bin", "gc_12_0_0_mec.bin",
            "gc_12_0_0_uni_mes.bin", "gc_12_0_0_imu.bin", "gc_12_0_0_rlc.bin"};
        CHECK(status == HSA_STATUS_SUCCESS && names == want && capacity == (8ull << 30),
              "converted protocol names firmware from GC 12.0.0 / SDMA 7.0.0 / MP 14.0.2");
        ConvertedRPC gfx11; gfx11.gc[0] = 11; gfx11.gc[1] = 0; gfx11.gc[2] = 0;
        gfx11.ips = {0x0b0000, 0x060000, 0x0d0000, 0x0d0000};
        CHECK(initializeDevice(gfx11, claimed, capacity) == HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS && gfx11.prepared.empty(),
              "converted protocol has no GC 11 load sequence and loads nothing");
    }

    // Metadata target IDs: an empty triple environment may be spelled
    // "unknown" (HRX System's prebuilt gfx12-generic device library is), and
    // nothing else about the ID may differ.
    CHECK(sameTargetId("amdgcn-amd-amdhsa--gfx12-generic", "amdgcn-amd-amdhsa--gfx12-generic"), "identical target IDs match");
    CHECK(sameTargetId("amdgcn-amd-amdhsa-unknown-gfx12-generic", "amdgcn-amd-amdhsa--gfx12-generic"),
          "unknown environment matches an empty one");
    CHECK(sameTargetId("amdgcn-amd-amdhsa-unknown-gfx90a:xnack-", "amdgcn-amd-amdhsa--gfx90a:xnack-"),
          "unknown environment matches with feature suffixes");
    CHECK(!sameTargetId("amdgcn-amd-amdhsa-unknown-gfx11-generic", "amdgcn-amd-amdhsa--gfx12-generic"),
          "a different processor does not match");
    CHECK(!sameTargetId("amdgcn-amd-amdhsa-gnu-gfx12-generic", "amdgcn-amd-amdhsa--gfx12-generic"),
          "a named environment does not match");
    CHECK(!sameTargetId("amdgcn-amd-amdhsa-unknown-gfx90a:xnack+", "amdgcn-amd-amdhsa--gfx90a:xnack-"),
          "a different feature does not match");

    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
