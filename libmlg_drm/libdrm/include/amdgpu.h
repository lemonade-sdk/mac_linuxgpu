/* libdrm_amdgpu for mac_linuxgpu: the part of libdrm_amdgpu's API (amdgpu.h)
 * that Mesa's ac_linux_drm layer uses (device, buffer and GPU virtual
 * address management), over the libdrm of this directory.
 *
 * Names, types and semantics follow libdrm_amdgpu (MIT; Copyright 2014
 * Advanced Micro Devices, Inc.), whose declarations are reproduced here for
 * the subset implemented. */
#ifndef MLG_AMDGPU_H
#define MLG_AMDGPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMDGPU_TIMEOUT_INFINITE			0xffffffffffffffffull
#define AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE	(1 << 0)

enum amdgpu_bo_handle_type {
	amdgpu_bo_handle_type_gem_flink_name = 0,
	amdgpu_bo_handle_type_kms = 1,
	amdgpu_bo_handle_type_dma_buf_fd = 2,
	amdgpu_bo_handle_type_kms_noimport = 3,
};

enum amdgpu_gpu_va_range {
	amdgpu_gpu_va_range_general = 0
};

enum amdgpu_sw_info {
	amdgpu_sw_info_address32_hi = 0,
	amdgpu_sw_info_address_prt_wa_control_bit = 1,
};

typedef struct amdgpu_device *amdgpu_device_handle;
typedef struct amdgpu_context *amdgpu_context_handle;
typedef struct amdgpu_bo *amdgpu_bo_handle;
typedef struct amdgpu_va *amdgpu_va_handle;

struct amdgpu_bo_alloc_request {
	uint64_t alloc_size;
	uint64_t phys_alignment;
	uint32_t preferred_heap;
	uint64_t flags;
};

struct amdgpu_bo_metadata {
	uint64_t flags;
	uint64_t tiling_info;
	uint32_t size_metadata;
	uint32_t umd_metadata[64];
};

struct amdgpu_bo_info {
	uint64_t alloc_size;
	uint64_t phys_alignment;
	uint32_t preferred_heap;
	uint64_t alloc_flags;
	struct amdgpu_bo_metadata metadata;
};

struct amdgpu_bo_import_result {
	amdgpu_bo_handle buf_handle;
	uint64_t alloc_size;
};

/* A command submission's fence (the context is libdrm's; Mesa keeps its
 * own and leaves it unused). */
struct amdgpu_cs_fence {
	amdgpu_context_handle context;
	uint32_t ip_type;
	uint32_t ip_instance;
	uint32_t ring;
	uint64_t fence;
};

struct amdgpu_heap_info {
	uint64_t heap_size;
	uint64_t heap_usage;
	uint64_t max_allocation;
};

struct amdgpu_gpu_info {
	uint32_t asic_id;
	uint32_t chip_rev;
	uint32_t chip_external_rev;
	uint32_t family_id;
	uint64_t ids_flags;
	uint64_t max_engine_clk;
	uint64_t max_memory_clk;
	uint32_t num_shader_engines;
	uint32_t num_shader_arrays_per_engine;
	uint32_t avail_quad_shader_pipes;
	uint32_t max_quad_shader_pipes;
	uint32_t cache_entries_per_quad_pipe;
	uint32_t num_hw_gfx_contexts;
	uint32_t rb_pipes;
	uint32_t enabled_rb_pipes_mask;
	uint32_t gpu_counter_freq;
	uint32_t backend_disable[4];
	uint32_t mc_arb_ramcfg;
	uint32_t gb_addr_cfg;
	uint32_t gb_tile_mode[32];
	uint32_t gb_macro_tile_mode[16];
	uint32_t pa_sc_raster_cfg[4];
	uint32_t pa_sc_raster_cfg1[4];
	uint32_t cu_active_number;
	uint32_t cu_ao_mask;
	uint32_t cu_bitmap[4][4];
	uint32_t vram_type;
	uint32_t vram_bit_width;
	uint32_t ce_ram_size;
	uint32_t vce_harvest_config;
	uint32_t pci_rev_id;
};

/* Device. As in libdrm_amdgpu, initializing twice on descriptors of the
 * same open file returns the same, reference-counted, device; the device
 * keeps a duplicate of @fd. */
int amdgpu_device_initialize(int fd, uint32_t *major_version, uint32_t *minor_version,
			     amdgpu_device_handle *device_handle);
int amdgpu_device_initialize2(int fd, bool deduplicate_device, uint32_t *major_version,
			      uint32_t *minor_version, amdgpu_device_handle *device_handle);
int amdgpu_device_deinitialize(amdgpu_device_handle device_handle);
int amdgpu_device_get_fd(amdgpu_device_handle device_handle);
const char *amdgpu_get_marketing_name(amdgpu_device_handle dev);
int amdgpu_query_sw_info(amdgpu_device_handle dev, enum amdgpu_sw_info info, void *value);

/* Buffers. */
int amdgpu_bo_alloc(amdgpu_device_handle dev, struct amdgpu_bo_alloc_request *alloc_buffer,
		    amdgpu_bo_handle *buf_handle);
int amdgpu_bo_free(amdgpu_bo_handle buf_handle);
void amdgpu_bo_inc_ref(amdgpu_bo_handle bo);
int amdgpu_bo_export(amdgpu_bo_handle buf_handle, enum amdgpu_bo_handle_type type,
		     uint32_t *shared_handle);
int amdgpu_bo_import(amdgpu_device_handle dev, enum amdgpu_bo_handle_type type,
		     uint32_t shared_handle, struct amdgpu_bo_import_result *output);
int amdgpu_bo_cpu_map(amdgpu_bo_handle buf_handle, void **cpu);
int amdgpu_bo_cpu_unmap(amdgpu_bo_handle buf_handle);
int amdgpu_bo_query_info(amdgpu_bo_handle buf_handle, struct amdgpu_bo_info *info);
int amdgpu_bo_set_metadata(amdgpu_bo_handle buf_handle, struct amdgpu_bo_metadata *info);
uint32_t amdgpu_bo_get_handle(amdgpu_bo_handle buf_handle);
/* User memory as a buffer (AMDGPU_GEM_USERPTR): the driver's process
 * cannot reach this process's pages, so this fails with -ENOSYS. */
int amdgpu_create_bo_from_user_mem(amdgpu_device_handle dev, void *cpu, uint64_t size,
				   amdgpu_bo_handle *buf_handle);

/* GPU virtual address ranges. */
#define AMDGPU_VA_RANGE_32_BIT		0x1
#define AMDGPU_VA_RANGE_HIGH		0x2
#define AMDGPU_VA_RANGE_REPLAYABLE	0x4

int amdgpu_va_range_query(amdgpu_device_handle dev, enum amdgpu_gpu_va_range type,
			  uint64_t *start, uint64_t *end);
int amdgpu_va_range_alloc(amdgpu_device_handle dev, enum amdgpu_gpu_va_range va_range_type,
			  uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
			  uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle,
			  uint64_t flags);
int amdgpu_va_range_free(amdgpu_va_handle va_range_handle);
uint64_t amdgpu_va_get_start_addr(amdgpu_va_handle va_handle);

#ifdef __cplusplus
}
#endif

#endif
