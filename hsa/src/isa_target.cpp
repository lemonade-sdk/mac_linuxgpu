#include "isa_target.h"
#include <cstdio>
#include <cstring>

namespace mac_hsa {
namespace {
// LLVM AMDGPU generic processors (AMDGPUUsage "Generic Processors"). Every
// current member joined its family in generic version 1.
struct GenericFamily { const char *name; uint16_t mach; };
constexpr GenericFamily kGenericFamilies[] = {
    {"gfx9-generic", 0x051},   {"gfx9-4-generic", 0x05f}, {"gfx10-1-generic", 0x052},
    {"gfx10-3-generic", 0x053}, {"gfx11-generic", 0x054},  {"gfx12-generic", 0x059},
    {"gfx12-5-generic", 0x05b},
};

// Processors reachable from a KFD gfx_target_version, with their LLVM
// EF_AMDGPU_MACH value, generic family and target-ID features
// (llvm/BinaryFormat/ELF.h, AMDGPUUsage "Processors").
struct Processor {
    uint32_t gfxTargetVersion;
    uint16_t mach;
    const char *generic; // nullptr: LLVM defines no generic family for it
    bool xnack, sramecc;
};
constexpr Processor kProcessors[] = {
    {90000, 0x02c, "gfx9-generic", true, false},    // gfx900
    {90002, 0x02d, "gfx9-generic", true, false},    // gfx902
    {90004, 0x02e, "gfx9-generic", true, false},    // gfx904
    {90006, 0x02f, "gfx9-generic", true, true},     // gfx906
    {90008, 0x030, nullptr, true, true},            // gfx908
    {90009, 0x031, "gfx9-generic", true, false},    // gfx909
    {90010, 0x03f, nullptr, true, true},            // gfx90a
    {90012, 0x032, "gfx9-generic", true, false},    // gfx90c
    {90402, 0x04c, "gfx9-4-generic", true, true},   // gfx942
    {90500, 0x04f, "gfx9-4-generic", true, true},   // gfx950
    {100100, 0x033, "gfx10-1-generic", true, false}, // gfx1010
    {100101, 0x034, "gfx10-1-generic", true, false}, // gfx1011
    {100102, 0x035, "gfx10-1-generic", true, false}, // gfx1012
    {100103, 0x042, "gfx10-1-generic", true, false}, // gfx1013
    {100300, 0x036, "gfx10-3-generic", false, false}, // gfx1030
    {100301, 0x037, "gfx10-3-generic", false, false}, // gfx1031
    {100302, 0x038, "gfx10-3-generic", false, false}, // gfx1032
    {100303, 0x039, "gfx10-3-generic", false, false}, // gfx1033
    {100304, 0x03e, "gfx10-3-generic", false, false}, // gfx1034
    {100305, 0x03d, "gfx10-3-generic", false, false}, // gfx1035
    {100306, 0x045, "gfx10-3-generic", false, false}, // gfx1036
    {110000, 0x041, "gfx11-generic", false, false}, // gfx1100
    {110001, 0x046, "gfx11-generic", false, false}, // gfx1101
    {110002, 0x047, "gfx11-generic", false, false}, // gfx1102
    {110003, 0x044, "gfx11-generic", false, false}, // gfx1103
    {110500, 0x043, "gfx11-generic", false, false}, // gfx1150
    {110501, 0x04a, "gfx11-generic", false, false}, // gfx1151
    {110502, 0x055, "gfx11-generic", false, false}, // gfx1152
    {110503, 0x058, "gfx11-generic", false, false}, // gfx1153
    {110504, 0x057, nullptr, false, false},         // gfx1154
    {120000, 0x048, "gfx12-generic", false, false}, // gfx1200
    {120001, 0x04e, "gfx12-generic", false, false}, // gfx1201
    {120500, 0x049, "gfx12-5-generic", false, true}, // gfx1250
};

const Processor *findProcessor(uint32_t gfxTargetVersion) {
    for (const auto &processor : kProcessors)
        if (processor.gfxTargetVersion == gfxTargetVersion) return &processor;
    return nullptr;
}
const GenericFamily *findGeneric(const char *name) {
    if (!name) return nullptr;
    for (const auto &family : kGenericFamilies)
        if (!std::strcmp(family.name, name)) return &family;
    return nullptr;
}
std::string featureSuffix(TargetFeature sramecc, TargetFeature xnack) {
    // Target-ID features are listed in alphabetical order.
    std::string suffix;
    if (sramecc == TargetFeature::On) suffix += ":sramecc+";
    else if (sramecc == TargetFeature::Off) suffix += ":sramecc-";
    if (xnack == TargetFeature::On) suffix += ":xnack+";
    else if (xnack == TargetFeature::Off) suffix += ":xnack-";
    return suffix;
}
// Decoded e_flags. Code object v3 (EI_ABIVERSION 1) encodes features as single
// on/off bits; v4 and later use two-bit selectors.
struct ObjectTarget { uint16_t mach; TargetFeature xnack, sramecc; uint8_t genericVersion; };
bool decode(uint32_t flags, uint8_t abiVersion, ObjectTarget &out) {
    out.mach = uint16_t(flags & 0xff);
    out.genericVersion = uint8_t(flags >> 24);
    if (abiVersion < 1 || abiVersion > 4) return false;
    if (abiVersion == 1) {
        if (flags & ~0x3ffu) return false;
        out.xnack = (flags & 0x100) ? TargetFeature::On : TargetFeature::Off;
        out.sramecc = (flags & 0x200) ? TargetFeature::On : TargetFeature::Off;
        return true;
    }
    if (flags & 0x00fff000u) return false; // unknown feature bits
    constexpr TargetFeature selectors[] = {TargetFeature::Unsupported, TargetFeature::Any,
                                           TargetFeature::Off, TargetFeature::On};
    out.xnack = selectors[(flags >> 8) & 3];
    out.sramecc = selectors[(flags >> 10) & 3];
    return true;
}
const char *machName(uint16_t mach, std::string &storage) {
    for (const auto &family : kGenericFamilies) if (family.mach == mach) return family.name;
    for (const auto &processor : kProcessors)
        if (processor.mach == mach) { storage = processorName(processor.gfxTargetVersion); return storage.c_str(); }
    return nullptr;
}
bool featureCompatible(TargetFeature object, TargetFeature agent) {
    // ROCr Isa::IsCompatible: only an explicit On/Off request constrains.
    return (object != TargetFeature::On && object != TargetFeature::Off) || object == agent;
}
} // namespace

uint32_t gfxTargetVersionFromGCVersion(uint32_t major, uint32_t minor, uint32_t revision) {
    struct Entry { uint8_t major, minor, revision; uint32_t target; };
    // amdkfd/kfd_device.c kgd2kfd_probe, the IP-discovery (default) branch.
    constexpr Entry table[] = {
        {9, 0, 1, 90000},   {9, 1, 0, 90002},   {9, 2, 2, 90002},   {9, 2, 1, 90004},
        {9, 3, 0, 90012},   {9, 4, 0, 90006},   {9, 4, 1, 90008},   {9, 4, 2, 90010},
        {9, 4, 3, 90402},   {9, 4, 4, 90402},   {9, 5, 0, 90500},
        {10, 1, 10, 100100}, {10, 1, 2, 100101}, {10, 1, 1, 100102}, {10, 1, 3, 100103},
        {10, 1, 4, 100103}, {10, 3, 0, 100300}, {10, 3, 2, 100301}, {10, 3, 1, 100303},
        {10, 3, 4, 100302}, {10, 3, 5, 100304}, {10, 3, 3, 100305}, {10, 3, 6, 100306},
        {10, 3, 7, 100306},
        {11, 0, 0, 110000}, {11, 0, 1, 110003}, {11, 0, 4, 110003}, {11, 0, 2, 110002},
        {11, 0, 3, 110001}, {11, 5, 0, 110500}, {11, 5, 1, 110501}, {11, 5, 2, 110502},
        {11, 5, 3, 110503}, {11, 5, 4, 110504},
        {12, 0, 0, 120000}, {12, 0, 1, 120001}, {12, 1, 0, 120500},
    };
    for (const auto &entry : table)
        if (entry.major == major && entry.minor == minor && entry.revision == revision)
            return entry.target;
    return 0;
}

std::string processorName(uint32_t gfxTargetVersion) {
    char name[32];
    std::snprintf(name, sizeof(name), "gfx%u%x%x", gfxTargetVersion / 10000,
                  (gfxTargetVersion / 100) % 100, gfxTargetVersion % 100);
    return name;
}

bool resolveIsaTarget(uint32_t gfxTargetVersion, TargetFeature xnack,
                      TargetFeature sramecc, IsaTarget &out) {
    const auto *processor = findProcessor(gfxTargetVersion);
    if (!processor) return false;
    IsaTarget target;
    target.gfxTargetVersion = gfxTargetVersion;
    target.processor = processorName(gfxTargetVersion);
    target.mach = processor->mach;
    if (const auto *family = findGeneric(processor->generic)) {
        target.generic = family->name;
        target.genericMach = family->mach;
        target.genericMinVersion = 1;
    }
    // A mode the device did not report (Unsupported/Any on a processor that has
    // the feature) stays Any: only code objects that leave it unspecified load.
    const auto clamp = [](bool supported, TargetFeature reported) {
        if (!supported) return TargetFeature::Unsupported;
        return reported == TargetFeature::Unsupported ? TargetFeature::Any : reported;
    };
    target.xnack = clamp(processor->xnack, xnack);
    target.sramecc = clamp(processor->sramecc, sramecc);
    target.defaultWavefrontSize = gfxTargetVersion < 100000 ? 64 : 32;
    out = std::move(target);
    return true;
}

std::string IsaTarget::isaName() const {
    return "amdgcn-amd-amdhsa--" + processor + featureSuffix(sramecc, xnack);
}
std::string IsaTarget::genericIsaName() const {
    if (generic.empty()) return {};
    // A generic family carries only the features all of its processors share.
    TargetFeature genericSramecc = sramecc, genericXnack = xnack;
    for (const auto &processor : kProcessors) {
        if (!processor.generic || generic != processor.generic) continue;
        if (!processor.sramecc) genericSramecc = TargetFeature::Unsupported;
        if (!processor.xnack) genericXnack = TargetFeature::Unsupported;
    }
    return "amdgcn-amd-amdhsa--" + generic + featureSuffix(genericSramecc, genericXnack);
}

uint32_t endProgramEncoding(const IsaTarget &target) {
    return target.gfxTargetVersion >= 110000 ? 0xbfb00000u : 0xbf810000u;
}
bool hasRsrc1ClampAndIEEE(const IsaTarget &target) {
    return target.gfxTargetVersion < 120000;
}

bool codeObjectTargetName(uint32_t flags, uint8_t abiVersion, std::string &out) {
    ObjectTarget object{};
    if (!decode(flags, abiVersion, object)) return false;
    std::string storage;
    const char *name = machName(object.mach, storage);
    if (!name) return false;
    out = std::string("amdgcn-amd-amdhsa--") + name +
        (abiVersion == 1 ? std::string() : featureSuffix(object.sramecc, object.xnack));
    return true;
}

bool codeObjectTargetCompatible(uint32_t flags, uint8_t abiVersion, const IsaTarget &agent,
                                std::string *objectTarget, bool *generic) {
    ObjectTarget object{};
    if (!agent.mach || !decode(flags, abiVersion, object)) return false;
    bool isGeneric = false;
    if (object.mach == agent.mach) {
        if (object.genericVersion) return false; // only generic machines carry a version
    } else if (agent.genericMach && object.mach == agent.genericMach) {
        // Generic code objects require code object v6 (EI_ABIVERSION 4).
        if (abiVersion < 4 || object.genericVersion < agent.genericMinVersion) return false;
        isGeneric = true;
    } else return false;
    if (abiVersion == 1) {
        // v3 bits are meaningful only for processors that have the feature.
        if (agent.xnack != TargetFeature::Unsupported && !featureCompatible(object.xnack, agent.xnack)) return false;
        if (agent.sramecc != TargetFeature::Unsupported && !featureCompatible(object.sramecc, agent.sramecc)) return false;
    } else {
        if (agent.xnack == TargetFeature::Unsupported && object.xnack != TargetFeature::Unsupported &&
            object.xnack != TargetFeature::Any) return false;
        if (agent.sramecc == TargetFeature::Unsupported && object.sramecc != TargetFeature::Unsupported &&
            object.sramecc != TargetFeature::Any) return false;
        if (!featureCompatible(object.xnack, agent.xnack) || !featureCompatible(object.sramecc, agent.sramecc))
            return false;
    }
    if (objectTarget && !codeObjectTargetName(flags, abiVersion, *objectTarget)) return false;
    if (generic) *generic = isGeneric;
    return true;
}

} // namespace mac_hsa
