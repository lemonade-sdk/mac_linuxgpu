#pragma once
#include <cstdint>
#include <string>

// GPU ISA identity derived from what the device reports, the way ROCr derives
// it from KFD topology: KFD's gfx_target_version (major*10000 + minor*100 +
// stepping) names the processor ("gfx" + major + hex(minor) + hex(stepping)),
// the processor selects its LLVM generic family, and the agent's xnack/sramecc
// modes complete the target ID. Nothing here is specific to one device: the
// tables below list every processor the upstream KFD maps from a GC IP version.
namespace mac_hsa {

// A target-ID feature, with the ELF (code object v4+) encoding semantics.
enum class TargetFeature : uint8_t { Unsupported, Any, Off, On };

struct IsaTarget {
    uint32_t gfxTargetVersion = 0;   // KFD encoding, e.g. 110000 for gfx1100
    std::string processor;           // "gfx1100"
    uint16_t mach = 0;               // EF_AMDGPU_MACH_* of the processor
    std::string generic;             // "gfx11-generic", empty when none exists
    uint16_t genericMach = 0;
    uint8_t genericMinVersion = 0;   // first generic version listing the processor
    TargetFeature xnack = TargetFeature::Unsupported;
    TargetFeature sramecc = TargetFeature::Unsupported;
    uint32_t defaultWavefrontSize = 0; // 64 on GFX9, 32 on GFX10 and later

    // Full HSA ISA names (ROCr format), e.g.
    // "amdgcn-amd-amdhsa--gfx90a:sramecc+:xnack-" and
    // "amdgcn-amd-amdhsa--gfx9-generic:xnack-".
    std::string isaName() const;
    std::string genericIsaName() const;
};

// KFD's GC IP version -> gfx_target_version mapping (kgd2kfd_probe in
// amdkfd/kfd_device.c). Returns 0 for an IP version the KFD does not map.
uint32_t gfxTargetVersionFromGCVersion(uint32_t major, uint32_t minor, uint32_t revision);
// "gfx" + major + hex(minor) + hex(stepping), as ROCr builds the name.
std::string processorName(uint32_t gfxTargetVersion);

// Resolves the processor named by gfxTargetVersion. The device-reported xnack
// and sramecc modes are clamped to what the processor supports: a processor
// without the feature always reports Unsupported. Returns false for a
// processor this runtime has no ELF machine or generic-family entry for.
bool resolveIsaTarget(uint32_t gfxTargetVersion, TargetFeature xnack,
                      TargetFeature sramecc, IsaTarget &out);

// Code-object target check, following LLVM's AMDGPU target-ID and generic
// processor rules (AMDGPUUsage "Target ID", "Generic Processors"):
//  * e_machine machine == the agent's processor, or
//  * machine == the agent's generic family, the object is code object v6+
//    (EI_ABIVERSION >= 4) and its generic version >= the version in which the
//    agent's processor joined that family;
//  * an object that requests xnack/sramecc On or Off must match the agent;
//    Any and Unsupported always match.
// On success, objectTarget receives the object's own target ID string (the
// value its "amdhsa.target" metadata must carry).
bool codeObjectTargetCompatible(uint32_t elfFlags, uint8_t abiVersion,
                                const IsaTarget &agent, std::string *objectTarget = nullptr,
                                bool *generic = nullptr);
// s_endpgm machine encoding (SOPP): GFX9/GFX10 0xbf810000, GFX11 and later
// 0xbfb00000 (llvm-mc -show-encoding for each processor in the table).
uint32_t endProgramEncoding(const IsaTarget &target);
// COMPUTE_PGM_RSRC1 DX10_CLAMP (bit 21) and IEEE_MODE (bit 23) exist through
// GFX11; GFX12 removed both bits.
bool hasRsrc1ClampAndIEEE(const IsaTarget &target);

// The target ID an object's e_flags describe, independent of any agent.
// Returns false for an unknown machine.
bool codeObjectTargetName(uint32_t elfFlags, uint8_t abiVersion, std::string &out);

} // namespace mac_hsa
