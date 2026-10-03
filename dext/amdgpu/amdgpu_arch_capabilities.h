#pragma once
#include <stdint.h>
namespace amdgpu { struct IPVersion { uint8_t major, minor, rev; }; }

namespace amdgpu {
// Per-queue scratch is runtime state on every family since GFX9: the CP's
// AQL firmware reads amd_queue_t's compute_tmpring_size, scratch SRD and
// backing address, which ROCr fills (AqlQueue's FillComputeTmpRingSize*
// and the SQ_BUF_RSRC fill). Upstream KFD and amdgpu program none of it;
// their MQD managers leave compute_tmpring_size zero and
// set_scratch_backing_va exists for GFX7/8 only. So the register layout
// comes from upstream (COMPUTE_TMPRING_SIZE's fields per family, carried
// by rt_queue_geometry.tmpring from the family's gc header), and only what
// no upstream header defines comes from ROCr: the WAVESIZE unit, whether
// WAVES counts per XCC or per shader engine, and the buffer descriptor
// layout. A family without an entry, or whose SRD layout is not encoded
// below, runs AQL queues without scratch.
enum class ScratchSRDLayout : uint8_t { Gfx9, Gfx10, Gfx11, Gfx12 };
struct ScratchArchitectureCapabilities {
    uint8_t gfxMajor;
    uint16_t waveSizeUnitBytes;
    uint8_t srdAddressBits, srdRecordBits;
    // Legacy WAVES counts are per XCC; GFX11+ counts are per shader engine.
    bool wavesPerEngine;
    ScratchSRDLayout srdLayout;
};
constexpr ScratchArchitectureCapabilities kScratchArchitectures[] = {
    {9, 1024, 48, 32, false, ScratchSRDLayout::Gfx9},
    {10,1024, 48, 32, false, ScratchSRDLayout::Gfx10},
    {11, 256, 48, 32, true,  ScratchSRDLayout::Gfx11},
    {12, 256, 48, 32, true,  ScratchSRDLayout::Gfx12},
};
constexpr const ScratchArchitectureCapabilities *scratch_architecture(IPVersion ip) {
    for (const auto &capability:kScratchArchitectures)
        if (capability.gfxMajor==ip.major) return &capability;
    return nullptr;
}
// aql_scratch_metadata() encodes the GFX12 scratch SRD only; architectures
// with another descriptor layout run AQL queues without scratch.
constexpr bool aql_scratch_layout_supported(IPVersion ip) {
    const auto *capability=scratch_architecture(ip);
    return capability && capability->srdLayout==ScratchSRDLayout::Gfx12;
}
}
