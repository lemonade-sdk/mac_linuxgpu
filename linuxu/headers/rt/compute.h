#ifndef LINUXU_RT_COMPUTE_H
#define LINUXU_RT_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct amdgpu_device;
struct rt_compute_ctx;
struct rt_compute_bo;
struct rt_compute_fence;

enum rt_compute_domain {
	RT_COMPUTE_GTT = 2,
	RT_COMPUTE_VRAM = 3,
};

struct rt_compute_properties {
	uint64_t vram_bytes;
	uint64_t visible_vram_bytes;
	uint64_t vram_start;
	uint64_t gart_start;
	uint64_t gart_bytes;
	uint64_t timestamp_frequency_hz;
	uint32_t gfx_ip_version;
	uint32_t ip_versions[4];
	uint32_t pci_domain;
	uint32_t pci_bus;
	uint32_t pci_device;
	uint32_t pci_function;
	uint32_t pci_chip_id;
	uint32_t pci_revision;
	uint32_t chip_revision;
	uint32_t shader_engines;
	uint32_t shader_arrays_per_engine;
	uint32_t cu_count;
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t wavefront_size;
	uint32_t scratch_slots_per_cu;
	uint32_t cu_bitmap[4][4];
	uint32_t gpu_copy_ready;
	uint32_t host_memory_verified;
};

/* ROCr target-feature modes (xnack, sramecc). */
enum rt_target_feature {
	RT_TARGET_FEATURE_UNSUPPORTED = 0,
	RT_TARGET_FEATURE_ANY = 1,
	RT_TARGET_FEATURE_OFF = 2,
	RT_TARGET_FEATURE_ON = 3,
};

/* The KFD topology node properties of the device's first node, as ROCr
 * reads them on Linux. Cache sizes are bytes of the first data cache KFD
 * reports per level (0 if none); product_name is the FRU board name,
 * NUL-padded (all zero without a FRU EEPROM). */
struct rt_compute_topology {
	uint32_t gfx_target_version;
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t lds_bytes;
	uint32_t scratch_slots_per_cu;
	uint32_t max_engine_clock_mhz;
	uint32_t xnack, sramecc; /* enum rt_target_feature */
	uint32_t xcc_count;
	uint64_t l1_bytes, l2_bytes, l3_bytes;
	char product_name[64];
};

/* The GC geometry and what upstream left active (QueryInfo tag 8). */
#define RT_DEVICE_SPEC_GEOMETRY		(1u << 0)
#define RT_DEVICE_SPEC_CUS		(1u << 1)
#define RT_DEVICE_SPEC_SHADER_ARRAYS	(1u << 2)
#define RT_DEVICE_SPEC_SA_DISABLE	(1u << 3)
#define RT_DEVICE_SPEC_BACKENDS		(1u << 4)
struct rt_device_spec {
	uint32_t present;	/* RT_DEVICE_SPEC_* */
	uint32_t shader_engines, shader_arrays_per_se, backends_per_se, cus_per_array;
	uint32_t wavefront_size, max_waves_per_simd, scratch_slots_per_cu, lds_bytes;
	uint32_t active_cus;
	uint32_t cu_bitmap[4][4];
	uint32_t active_sa_bitmap;
	uint32_t cc_sa_disable, user_sa_disable;
	uint32_t active_rb_bitmap, active_rbs;
};
int rt_device_spec(struct amdgpu_device *adev, struct rt_device_spec *out);

struct rt_compute_bo_info {
	uint64_t size;
	uint64_t gpu_address;
	uint64_t dma_address;
	void *cpu_address;
	uint32_t domain;
	uint32_t shared_descriptor_available;
};

/* One upstream TTM snapshot. usable_bytes subtracts VRAM pinned
 * before this client opened and the VM reserve. free_bytes reflects current
 * VRAM manager usage, including this client's pinned BOs. */
struct rt_compute_memory_usage {
	uint64_t total_bytes;
	uint64_t usable_bytes;
	uint64_t used_bytes;
	uint64_t free_bytes;
	uint64_t visible_bytes;
	uint64_t visible_used_bytes;
};

/* The caller must quiesce queues and all BO/fence users before close.
 * All calls on one context are serialized by its internal lock. */
int rt_compute_open(struct pci_dev *pdev, struct rt_compute_ctx **out);
int rt_compute_status(struct rt_compute_ctx *ctx);
/* Explicit GPU readback probe. This submits two real SDMA copies and bounds
 * each fence wait. Success is required before status reports ready. */
int rt_compute_verify_host_memory(struct rt_compute_ctx *ctx);
int rt_compute_properties(struct rt_compute_ctx *ctx,
			  struct rt_compute_properties *out);
int rt_compute_memory_usage(struct rt_compute_ctx *ctx,
			    struct rt_compute_memory_usage *out);
struct amdgpu_device *rt_compute_device(struct rt_compute_ctx *ctx);
/* -ENODEV until KFD has added the device to its topology. */
int rt_device_topology(struct amdgpu_device *adev, struct rt_compute_topology *out);
int rt_compute_close(struct rt_compute_ctx *ctx);
/* Hold DMA releases while an engine of @adev is stalled (a job running past
 * its timeout) so late GPU work never reaches memory released meanwhile
 * (linuxu_dart_set_hold); NULL detaches and releases what was held.
 * rt_compute_open attaches the device and rt_compute_close detaches it. */
void rt_dma_hold_attach(struct amdgpu_device *adev);

/* GTT returns a coherent CPU/DART backing bound at a pinned upstream GART
 * address. The CPU address can be passed to the dext DMA descriptor exporter;
 * it remains valid until rt_compute_bo_free. VRAM CPU transfers use coherent
 * GTT staging and a bounded upstream SDMA copy. */
int rt_compute_bo_alloc(struct rt_compute_ctx *ctx, uint64_t size,
			uint64_t alignment, enum rt_compute_domain domain,
			struct rt_compute_bo **out);
int rt_compute_bo_info(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			struct rt_compute_bo_info *out);
int rt_compute_bo_read(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			uint64_t offset, void *dst, size_t size);
int rt_compute_bo_write(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
			 uint64_t offset, const void *src, size_t size);
int rt_compute_bo_copy(struct rt_compute_ctx *ctx, struct rt_compute_bo *src,
			struct rt_compute_bo *dst, uint64_t src_offset,
			uint64_t dst_offset, uint32_t size,
			struct rt_compute_fence **out_fence);
int rt_compute_bo_free(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo);

/* A fence exists only for a completed upstream SDMA copy job. An uncertain
 * submission poisons the context; close/free then return EBUSY and retain
 * backing memory until the device lifecycle can safely quiesce it. */
int rt_compute_fence_wait(struct rt_compute_fence *fence);
void rt_compute_fence_put(struct rt_compute_fence *fence);

#ifdef __cplusplus
}
#endif
#endif
