#pragma once
#include "amdgpu_aql_abi.h"
#include "amdgpu_dispatch_abi.h"
#include "amdgpu_aql_resources.h"
#include "../../hsa/third_party/hsa/include/hsa/amd_hsa_signal.h"
#include <stddef.h>
#include <string.h>

namespace amdgpu {
// One 16 KiB VRAM block per queue. The MQD region holds whatever upstream
// KFD's compute MQD manager for the device builds there (rt_queue_build_mqd);
// a device whose aligned MQD exceeds kAQLMQDBytes is reported unsupported.
constexpr uint32_t kAQLStorageBytes = 16384, kAQLEOPOffset = 4096;
constexpr uint32_t kAQLMQDBytes = kAQLEOPOffset, kAQLEOPBytes = 4096;
constexpr uint32_t kAQLRingOffset = 8192, kAQLRingBytes = 4096;
constexpr uint32_t kAQLCompletionOffset = 12288, kAQLInactiveOffset = 12352;
constexpr uint32_t kAQLMetadataOffset = 12416;
// Persistent queue ring sizes (packets, powers of two) dext_aql_create takes.
constexpr uint32_t kAQLQueueMinPackets = 64, kAQLQueueMaxPackets = 4096;
static_assert(kAQLEOPOffset + kAQLEOPBytes <= kAQLRingOffset);
static_assert(kAQLMetadataOffset + sizeof(amd_queue_t) <= kAQLStorageBytes);
static_assert(sizeof(amd_signal_t) == 64 && sizeof(hsa_kernel_dispatch_packet_t) == 64);

// AMDHSA kernel descriptor (LLVM AMDHSAKernelDescriptor.h, code object v3+):
// the 64-byte object an AQL dispatch packet's kernel_object points to. The
// CP reads the entry offset, PGM_RSRC1/2/3 and the kernel code properties.
struct AQLKernelDescriptor {
    uint32_t group_segment_fixed_size, private_segment_fixed_size, kernarg_size;
    uint8_t reserved0[4];
    int64_t kernel_code_entry_byte_offset;
    uint8_t reserved1[20];
    uint32_t compute_pgm_rsrc3, compute_pgm_rsrc1, compute_pgm_rsrc2;
    uint16_t kernel_code_properties, kernarg_preload;
    uint8_t reserved3[4];
};
static_assert(sizeof(AQLKernelDescriptor) == 64);
static_assert(offsetof(AQLKernelDescriptor, kernel_code_entry_byte_offset) == 16);
static_assert(offsetof(AQLKernelDescriptor, compute_pgm_rsrc3) == 44);
static_assert(offsetof(AQLKernelDescriptor, kernel_code_properties) == 56);
// kernel_code_properties (AMDHSA code object ABI).
constexpr uint16_t kAQLEnableSGPRKernargSegmentPtr = 1u << 3;
constexpr uint16_t kAQLEnableWavefrontSize32 = 1u << 10;
// A descriptor built for a raw code launch lives after the metadata.
constexpr uint32_t kAQLDescriptorOffset =
    (kAQLMetadataOffset + sizeof(amd_queue_t) + 63) & ~63u;
static_assert(kAQLDescriptorOffset + sizeof(AQLKernelDescriptor) <= kAQLStorageBytes);

namespace detail {
// Signals, metadata and one dispatch packet; the MQD region stays zeroed
// for rt_queue_build_mqd. The storage has been zeroed and validated.
inline void aql_fill_ring(uint8_t *bytes, uint64_t base, uint64_t descriptor,
    uint64_t kernarg, const uint32_t groups[3], const uint32_t threads[3],
    uint32_t cuCount, uint32_t wavesPerCU) {
    auto &metadata=*reinterpret_cast<amd_queue_t *>(bytes+kAQLMetadataOffset);
    metadata.hsa_queue.type=HSA_QUEUE_TYPE_SINGLE;
    metadata.hsa_queue.features=HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
    metadata.hsa_queue.base_address=reinterpret_cast<void *>(base+kAQLRingOffset);
    metadata.hsa_queue.size=kAQLRingBytes/64;
    metadata.read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
    metadata.max_cu_id=cuCount-1;
    metadata.group_segment_aperture_base_hi=kAQLGroupApertureHi;
    metadata.private_segment_aperture_base_hi=kAQLPrivateApertureHi;
    metadata.max_wave_id=wavesPerCU-1; // GC discovery scratch-slot limit.
    metadata.queue_properties=AMD_QUEUE_PROPERTIES_IS_PTR64;
    metadata.queue_inactive_signal.handle=base+kAQLInactiveOffset;
    auto &done=*reinterpret_cast<amd_signal_t *>(bytes+kAQLCompletionOffset);
    done.kind=AMD_SIGNAL_KIND_USER; done.value=1;
    auto &inactive=*reinterpret_cast<amd_signal_t *>(bytes+kAQLInactiveOffset);
    inactive.kind=AMD_SIGNAL_KIND_USER;
    auto *packets=reinterpret_cast<hsa_kernel_dispatch_packet_t *>(bytes+kAQLRingOffset);
    for (unsigned i=0;i<kAQLRingBytes/64;++i) packets[i].header=HSA_PACKET_TYPE_INVALID;
    auto &p=packets[0];
    p.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    p.setup=3<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    p.workgroup_size_x=uint16_t(threads[0]); p.workgroup_size_y=uint16_t(threads[1]); p.workgroup_size_z=uint16_t(threads[2]);
    p.grid_size_x=groups[0]*threads[0]; p.grid_size_y=groups[1]*threads[1]; p.grid_size_z=groups[2]*threads[2];
    p.kernel_object=descriptor; p.kernarg_address=reinterpret_cast<void *>(kernarg);
    p.completion_signal.handle=base+kAQLCompletionOffset;
}
inline bool aql_storage_valid(const void *storage, uint64_t base, uint32_t cuCount,
                              uint32_t wavesPerCU) {
    return storage && !(reinterpret_cast<uintptr_t>(storage) & 63) && base &&
        !(base & 16383) && base <= (1ull << 48) - kAQLStorageBytes &&
        cuCount && wavesPerCU;
}
}

// Ring, signals and metadata for one bounded dispatch of a code object
// kernel; the MQD region is left zeroed for rt_queue_build_mqd.
inline bool aql_build_storage(void *storage, uint64_t base, uint64_t descriptor,
    uint64_t kernarg, const AQLDispatchRequest &r, uint32_t cuCount, uint32_t wavesPerCU) {
    if (!detail::aql_storage_valid(storage, base, cuCount, wavesPerCU) ||
        !aql_dispatch_shape(r) || !descriptor || (descriptor & 63) ||
        descriptor > (1ull << 48) - 64 || !kernarg || (kernarg & 15) ||
        kernarg >= (1ull << 48) || r.kernargBytes > (1ull << 48) - kernarg) return false;
    memset(storage, 0, kAQLStorageBytes);
    detail::aql_fill_ring(static_cast<uint8_t *>(storage), base, descriptor, kernarg,
                          r.groups, r.threads, cuCount, wavesPerCU);
    return true;
}

// A raw shader launch (selector 51's ComputeDispatchRequest) as an AQL
// dispatch: code address, PGM_RSRC1/2/3 and the user SGPRs the request
// gives. AQL loads user SGPRs from what the kernel code properties enable;
// two user SGPRs are the kernarg segment pointer (the AMDHSA layout the
// request's producer uses), none is a launch without arguments. Other
// counts cannot be expressed. LDS is sized from the packet's group segment
// in AQL, so a request must leave PGM_RSRC2.LDS_SIZE to it (zero, as
// AMDHSA code objects do). Wave32, as the request ABI specifies.
struct AQLCodeLaunch {
    uint64_t code, kernarg;
    uint32_t rsrc1, rsrc2, rsrc3;
    uint16_t properties;
    uint32_t groups[3], threads[3];
};
inline bool aql_code_launch(const ComputeDispatchRequest &r, uint64_t code,
                            bool rsrc1ClampAndIEEE, AQLCodeLaunch &out) {
    constexpr uint32_t ldsSizeMask = 0x1ffu << 15; // COMPUTE_PGM_RSRC2.LDS_SIZE
    if (!compute_dispatch_shape(r, rsrc1ClampAndIEEE) ||
        (r.userSGPRCount != 0 && r.userSGPRCount != 2) ||
        (r.rsrc2 & ldsSizeMask) || !code || (code & 255) || code >= (1ull << 48))
        return false;
    out = {};
    for (unsigned i = 0; i < 3; ++i) {
        if (uint64_t(r.groups[i]) * r.threads[i] > UINT32_MAX) return false;
        out.groups[i] = r.groups[i]; out.threads[i] = r.threads[i];
    }
    out.code = code;
    out.rsrc1 = r.rsrc1; out.rsrc2 = r.rsrc2; out.rsrc3 = r.rsrc3;
    out.properties = kAQLEnableWavefrontSize32;
    if (r.userSGPRCount == 2) {
        out.properties |= kAQLEnableSGPRKernargSegmentPtr;
        out.kernarg = uint64_t(r.userSGPR[0]) | (uint64_t(r.userSGPR[1]) << 32);
    }
    return true;
}
inline bool aql_build_code_storage(void *storage, uint64_t base, const AQLCodeLaunch &l,
                                   uint32_t cuCount, uint32_t wavesPerCU) {
    if (!detail::aql_storage_valid(storage, base, cuCount, wavesPerCU) ||
        !l.code || (l.code & 255) || l.code >= (1ull << 48)) return false;
    memset(storage, 0, kAQLStorageBytes);
    auto *bytes = static_cast<uint8_t *>(storage);
    const uint64_t descriptor = base + kAQLDescriptorOffset;
    AQLKernelDescriptor d{};
    d.kernel_code_entry_byte_offset = int64_t(l.code) - int64_t(descriptor);
    d.compute_pgm_rsrc1 = l.rsrc1; d.compute_pgm_rsrc2 = l.rsrc2; d.compute_pgm_rsrc3 = l.rsrc3;
    d.kernel_code_properties = l.properties;
    memcpy(bytes + kAQLDescriptorOffset, &d, sizeof(d));
    detail::aql_fill_ring(bytes, base, descriptor, l.kernarg, l.groups, l.threads,
                          cuCount, wavesPerCU);
    return true;
}
}
