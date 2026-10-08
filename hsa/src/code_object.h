#pragma once
#include "isa_target.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mac_hsa {
struct KernelMetadata {
    std::string name, symbol;
    uint64_t descriptor = 0, entry = 0;
    uint32_t kernargSize = 0, kernargAlignment = 0, groupSize = 0, privateSize = 0;
    uint32_t rsrc1 = 0, rsrc2 = 0, rsrc3 = 0;
    uint16_t properties = 0, preload = 0;
    bool dynamicStack = false;
};
struct Relocation {
    uint64_t offset, symbol;
    int64_t addend;
    uint32_t type;
    bool absolute;
};
struct CodeObject {
    struct Segment { uint64_t offset, size, fileOffset, fileSize; uint32_t flags; };
    uint64_t virtualBase = 0;
    // The largest alignment of its loadable segments: the image's device
    // address must be a multiple of it (relocation keeps segment offsets).
    uint64_t alignment = 1;
    std::vector<uint8_t> image;
    std::vector<Segment> segments;
    std::vector<KernelMetadata> kernels;
    std::vector<Relocation> relocations;
};
// Bounded ELF64 AMDHSA loader. Linked code objects only. The object's target
// must be compatible with the agent's ISA: the agent's own processor or its
// generic family (codeObjectTargetCompatible). Incompatible targets, imports,
// TLS and unsupported relocation forms are rejected before GPU upload.
// A kernel's wavefront size must be one the agent supports (32 needs GFX10+).
bool parseCodeObject(std::span<const uint8_t> file, CodeObject &output, const IsaTarget &agent);
// The e_flags/EI_ABIVERSION of an AMDHSA ELF header, without parsing further.
bool codeObjectHeader(std::span<const uint8_t> file, uint32_t &flags, uint8_t &abiVersion);
bool relocateCodeObject(CodeObject &object, uint64_t gpuAddress);
// Target IDs compare equal when they differ only in how the triple spells an
// empty environment: LLVM normalizes amdgcn-amd-amdhsa- and
// amdgcn-amd-amdhsa-unknown- to the same triple, and toolchains write either
// (HRX System's prebuilt device library says amdgcn-amd-amdhsa-unknown-).
bool sameTargetId(const std::string &actual, const std::string &expected);
}
