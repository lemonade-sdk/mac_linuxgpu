/* DriverKit AQL ownership over upstream MES queue operations. */
#ifndef LINUXU_RT_QUEUE_H
#define LINUXU_RT_QUEUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct amdgpu_device;

/* Upper bound on first-MEC HQDs: the size of amdgpu's per-MEC queue bitmap
 * (AMDGPU_MAX_COMPUTE_QUEUES); queue.c asserts the two agree. */
#define RT_QUEUE_MAX_SLOTS 128u
#define RT_QUEUE_MASK_WORDS (RT_QUEUE_MAX_SLOTS / 64u)

/* COMPUTE_TMPRING_SIZE's WAVES and WAVESIZE fields as the family's upstream
 * gc register header defines them (all zero: family without a known
 * layout, so AQL queues run without scratch). */
struct rt_tmpring_layout {
    uint32_t waves_mask, waves_shift;
    uint32_t wave_size_mask, wave_size_shift;
};

/* Device properties the AQL metadata and scratch setup consume, read from
 * upstream's discovery results (GC IP version, gfx.config, gfx.cu_info).
 * mqd_bytes is the space upstream KFD's MQD manager needs for one MQD
 * (AMDGPU_MQD_SIZE_ALIGN of its mqd_size). */
struct rt_queue_geometry {
    uint32_t gfx_major, gfx_minor, gfx_revision;
    uint32_t engines, compute_units, waves_per_cu;
    uint32_t mqd_bytes;
    struct rt_tmpring_layout tmpring;
};

/* First-MEC HQD ownership as bitmaps (bit = pipe * queues_per_pipe + queue,
 * as amdgpu_gfx_mec_queue_to_bit() numbers them; word = bit / 64).
 * kernel_mask: upstream kernel compute rings (mec_bitmap before partition).
 * mes_mask:    HQDs SET_HW_RESOURCES gave the MES scheduler (KFD, user queues).
 * dext_mask:   legacy HQDs reserved for DriverKit AQL queues.
 * kfd_mask:    KFD's current cp_queue_bitmap copy (0 without KFD).
 * owned_mask:  dext_mask entries currently reserved.
 * doorbells:   dword doorbell index per dext_mask slot, 0 elsewhere. */
struct rt_queue_partition {
    uint32_t pipes, queues_per_pipe;
    uint64_t kernel_mask[RT_QUEUE_MASK_WORDS], mes_mask[RT_QUEUE_MASK_WORDS];
    uint64_t dext_mask[RT_QUEUE_MASK_WORDS], kfd_mask[RT_QUEUE_MASK_WORDS];
    uint64_t owned_mask[RT_QUEUE_MASK_WORDS];
    uint32_t doorbells[RT_QUEUE_MAX_SLOTS];
};

static inline int rt_queue_mask_test(const uint64_t *mask, uint32_t slot)
{
    return slot < RT_QUEUE_MAX_SLOTS && ((mask[slot / 64u] >> (slot % 64u)) & 1u);
}

/* GPU addresses and sizes of one AQL queue, in the units upstream KFD's
 * struct queue_properties uses (bytes; sizes are powers of two). */
struct rt_queue_mqd {
    uint64_t mqd_address;
    uint64_t ring_address;
    uint64_t read_pointer;   /* amd_queue_t read_dispatch_id */
    uint64_t write_pointer;  /* amd_queue_t write_dispatch_id */
    uint64_t eop_address;
    uint32_t ring_bytes, eop_bytes;
};

/* -ENODEV without MES legacy-queue mapping or KFD; -EOPNOTSUPP when a
 * capability the partition relies on is absent (multiple XCCs). */
int rt_queue_geometry(struct amdgpu_device *, struct rt_queue_geometry *);
/* The device part of the geometry alone (mqd_bytes 0): what AQL metadata
 * and scratch need for a queue any scheduler runs, legacy HQD or not.
 * -EINVAL when discovery left a count zero. */
int rt_queue_device_geometry(struct amdgpu_device *, struct rt_queue_geometry *);
/* Idempotent. Reserves the legacy HQDs no upstream ring or MES scheduler
 * uses, marks them kernel-owned in mec_bitmap and removes them from KFD's
 * cp_queue_bitmap. -ENOSPC when no such HQD exists. */
int rt_queue_partition(struct amdgpu_device *, struct rt_queue_partition *out);
int rt_queue_reserve(struct amdgpu_device *, uint32_t *slot, uint32_t *doorbell);
void rt_queue_release(struct amdgpu_device *, uint32_t slot);
/* Writes the AQL compute MQD for a reserved slot into mqd (mqd_bytes long,
 * at least rt_queue_geometry().mqd_bytes): upstream KFD's kernel-queue MQD
 * manager for this device builds it, and the legacy kernel queue fields
 * (doorbell enable, inactive HQD, no CWSR, no debugger) come from amdgpu's
 * own kernel compute queue MQD builder, so MES MAP_LEGACY_QUEUE receives
 * what amdgpu gives it for its kernel compute rings. */
int rt_queue_build_mqd(struct amdgpu_device *, uint32_t slot, uint32_t doorbell,
                       const struct rt_queue_mqd *, void *mqd, size_t mqd_bytes);
/* Make the MQD that rt_queue_build_mqd wrote start at AQL packet @packet
 * of a ring of @ring_bytes (a queue mapped again after a device reset
 * resumes where its producer is). 0 or -EINVAL. */
int rt_queue_mqd_set_position(void *mqd, uint64_t ring_bytes, uint64_t packet);
int rt_queue_map(struct amdgpu_device *, uint32_t slot, uint32_t doorbell,
                 uint64_t mqd, uint64_t write_pointer);
int rt_queue_unmap(struct amdgpu_device *, uint32_t slot, uint32_t doorbell);
int rt_queue_kick(struct amdgpu_device *, uint32_t doorbell, uint64_t packet);
void rt_queue_flush(struct amdgpu_device *);
#ifdef __cplusplus
}
#endif
#endif
