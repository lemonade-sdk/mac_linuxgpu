#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../dext/amdgpu/amdgpu_aql_packets.h"

int main()
{
    using namespace amdgpu;
    alignas(64) unsigned char storage[kAQLStorageBytes + 64];
    const uint64_t base = 0x100000000ull;
    AQLDispatchRequest request{};
    request.version = 1;
    request.codeHandle = 1;
    request.kernargHandle = 2;
    request.kernargBytes = 32;
    request.timeoutUS = 100;
    for (unsigned i = 0; i < 3; i++) request.groups[i] = request.threads[i] = 1;
    memset(storage, 0xa5, sizeof(storage));
    assert(aql_build_storage(storage, base, base + 65536, base + 131072,
                             request, 8, 32));
    // The MQD region is left for upstream KFD's MQD manager to fill.
    for (unsigned i = 0; i < kAQLMQDBytes; i++) assert(storage[i] == 0);
    assert(!aql_build_storage(storage + 8, base, base + 65536, base + 131072,
                              request, 8, 32));
    assert(!aql_build_storage(storage, base, base + 65536, (1ull << 48) - 16,
                              request, 8, 32));
    assert(!aql_build_storage(storage, base + 4096, base + 65536, base + 131072,
                              request, 8, 32));
    auto *staging = static_cast<unsigned char *>(aligned_alloc(64, kAQLStorageBytes));
    assert(staging);
    assert(aql_build_storage(staging, base, base + 65536, base + 131072,
                             request, 8, 32));
    free(staging);

    // Selector 51 raw launches as AQL: a synthesized AMDHSA descriptor in
    // the storage block, user SGPRs as the kernarg segment pointer.
    ComputeDispatchRequest raw{};
    raw.version = 2; raw.codeHandle = 1; raw.codeBytes = 4;
    raw.groups[0] = 3; raw.groups[1] = raw.groups[2] = 1;
    raw.threads[0] = 64; raw.threads[1] = raw.threads[2] = 1;
    raw.rsrc1 = 0x000c0000u | (1u << 29); raw.rsrc3 = 0x10;
    raw.userSGPRCount = 2; raw.rsrc2 = (2u << 1) | (1u << 7);
    raw.userSGPR[0] = 0x00001000; raw.userSGPR[1] = 0x2;
    raw.timeoutUS = 100;
    const uint64_t code = base + 0x40000;
    AQLCodeLaunch launch{};
    assert(aql_code_launch(raw, code, false, launch));
    assert(launch.kernarg == 0x200001000ull && launch.code == code &&
           launch.properties == (kAQLEnableWavefrontSize32 | kAQLEnableSGPRKernargSegmentPtr) &&
           launch.rsrc1 == raw.rsrc1 && launch.rsrc2 == raw.rsrc2 && launch.rsrc3 == raw.rsrc3);
    memset(storage, 0xa5, sizeof(storage));
    assert(aql_build_code_storage(storage, base, launch, 8, 32));
    for (unsigned i = 0; i < kAQLMQDBytes; i++) assert(storage[i] == 0);
    AQLKernelDescriptor d;
    memcpy(&d, storage + kAQLDescriptorOffset, sizeof(d));
    assert(d.kernel_code_entry_byte_offset == int64_t(code) - int64_t(base + kAQLDescriptorOffset));
    assert(d.compute_pgm_rsrc1 == raw.rsrc1 && d.compute_pgm_rsrc2 == raw.rsrc2 &&
           d.compute_pgm_rsrc3 == raw.rsrc3 && d.kernel_code_properties == launch.properties &&
           !d.group_segment_fixed_size && !d.private_segment_fixed_size && !d.kernarg_preload);
    hsa_kernel_dispatch_packet_t p;
    memcpy(&p, storage + kAQLRingOffset, sizeof(p));
    assert((p.header & 255) == HSA_PACKET_TYPE_KERNEL_DISPATCH);
    assert(p.kernel_object == base + kAQLDescriptorOffset &&
           reinterpret_cast<uint64_t>(p.kernarg_address) == launch.kernarg &&
           p.grid_size_x == 192 && p.workgroup_size_x == 64 && p.grid_size_y == 1 &&
           p.completion_signal.handle == base + kAQLCompletionOffset);
    // A code object descriptor below the code works as well (negative offset).
    assert(aql_code_launch(raw, base + 0x1000, false, launch) &&
           aql_build_code_storage(storage, base + 0x100000, launch, 8, 32));
    memcpy(&d, storage + kAQLDescriptorOffset, sizeof(d));
    assert(d.kernel_code_entry_byte_offset < 0);
    // No user SGPRs: no kernarg pointer.
    raw.userSGPRCount = 0; raw.rsrc2 = 0; raw.userSGPR[0] = raw.userSGPR[1] = 0;
    assert(aql_code_launch(raw, code, false, launch) &&
           launch.properties == kAQLEnableWavefrontSize32 && !launch.kernarg);
    // Inexpressible launches.
    raw.userSGPRCount = 1; raw.rsrc2 = 1u << 1; raw.userSGPR[0] = 7;
    assert(!aql_code_launch(raw, code, false, launch));
    raw.userSGPRCount = 0; raw.rsrc2 = 1u << 15; raw.userSGPR[0] = 0; // LDS_SIZE
    assert(!aql_code_launch(raw, code, false, launch));
    raw.rsrc2 = 0; raw.groups[0] = 0x7fffffffu; raw.threads[0] = 4;
    assert(!aql_code_launch(raw, code, false, launch));
    raw.groups[0] = 1;
    assert(!aql_code_launch(raw, code + 4, false, launch) && !aql_code_launch(raw, 1ull << 48, false, launch));
    assert(aql_code_launch(raw, code, false, launch));
    // DX10_CLAMP/IEEE_MODE only where the target's RSRC1 has them.
    raw.rsrc1 |= 0x00a00000u;
    assert(!aql_code_launch(raw, code, false, launch) &&
           aql_code_launch(raw, code, true, launch) && launch.rsrc1 == raw.rsrc1);
    assert(!aql_build_code_storage(storage + 8, base, launch, 8, 32));
    assert(!aql_build_code_storage(storage, base + 4096, launch, 8, 32));

    // Scratch: COMPUTE_TMPRING_SIZE fields come from the family's gc header
    // (here gc_12_0_0 / gc_11_0_0 values); the WAVESIZE unit, per-engine
    // WAVES and the SRD from ROCr's runtime ABI.
    const TmpRingLayout gc12{0x00000fffu, 0, 0x3ffff000u, 12};
    const TmpRingLayout gc11{0x00000fffu, 0, 0x07fff000u, 12};
    const IPVersion gfx12{12, 0, 1}, gfx11{11, 0, 0};
    uint32_t aligned = 0, waves = 0; uint64_t bytes = 0;
    assert(aql_scratch_geometry(gfx12, gc12, 1000, 64, 4, 32, aligned, waves, bytes));
    assert(aligned == 1008 && waves == 512 && bytes == uint64_t(1008) * 64 * 512 * 4);
    amd_queue_t q{};
    const uint64_t scratch = 0x400000000ull;
    assert(aql_scratch_metadata(gfx12, gc12, q, scratch, bytes, aligned, 4, waves));
    assert(q.compute_tmpring_size == (512u | ((1008u * 64 / 256) << 12)));
    assert(q.scratch_backing_memory_location == scratch && q.scratch_wave64_lane_byte_size == 1008);
    // The WAVESIZE field width bounds the per-lane size: a narrower layout
    // refuses what fits gc12 (252 KiB lanes need 18 bits of 256-byte units).
    const TmpRingLayout narrow{0x00000fffu, 0, 0x0000f000u, 12};
    assert(aql_scratch_geometry(gfx12, gc12, 200000, 64, 4, 32, aligned, waves, bytes));
    assert(!aql_scratch_geometry(gfx12, narrow, 200000, 64, 4, 32, aligned, waves, bytes));
    // No layout from upstream, or a descriptor layout not encoded (GFX11):
    // the queue runs without scratch.
    assert(!aql_scratch_geometry(gfx12, TmpRingLayout{}, 1000, 64, 4, 32, aligned, waves, bytes));
    assert(!aql_scratch_geometry(gfx11, gc11, 1000, 64, 4, 32, aligned, waves, bytes));
    assert(!aql_scratch_metadata(gfx11, gc11, q, scratch, 1u << 20, 1008, 4, 32));
    // More waves than the WAVES field holds.
    assert(!aql_scratch_geometry(gfx12, gc12, 16, 4096, 1, 64, aligned, waves, bytes));
    puts("AQL storage: host alignment, complete GPU address ranges, raw launch descriptors "
         "and scratch encoding passed");
}
