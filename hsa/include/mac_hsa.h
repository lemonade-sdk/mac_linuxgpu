#pragma once
#include <hsa/hsa.h>
#include "mac_hsa_power.h" // device power: suspend, resume, lost device memory

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic ABI, independent of HSA extension numbering. This reports a live
// driver snapshot, not dispatch capability or HSA conformance.
typedef struct mac_hsa_device_info_s {
    uint64_t registry_id;
    uint64_t driver_build;
    uint64_t bringup_stage;
    uint64_t visible_vram_bytes;
    uint64_t total_vram_bytes;
    uint32_t gfx_major;
    uint32_t gfx_minor;
    uint32_t gfx_revision;
    uint32_t reserved;
} mac_hsa_device_info_t;

__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_get_driver_info(hsa_agent_t agent,
                                          mac_hsa_device_info_t *info,
                                          size_t info_size);

// The agent's GPU spec, from the driver's QueryInfo tag 8: upstream amdgpu's
// GC geometry and what it left active (session_state.h's struct
// mlg_device_spec). The block is 32 dwords:
//   [0]  header: the driver's spec version, 0 when the GC IP is not resolved
//   [1..8]  geometry: shader engines, shader arrays/SE, backends/SE,
//          CUs/array, wavefront size, max waves/SIMD, scratch slots/CU, LDS bytes
//   [9]  active CU count (harvested), [10..17] active CU bitmaps, as KFD's,
//          [10 + 2 * engine + array] for four engines of two arrays
//   [18] active SA bitmap, [19] GRBM_CC_GC_SA_UNIT_DISABLE raw,
//   [20] GRBM_GC_USER_SA_UNIT_DISABLE raw (GC 12; 0 elsewhere)
//   [21] active RB bitmap, [22] active RB count
//   [23..27] 0: SH_MEM_CONFIG/BASES, the shader-array config registers and
//          GRBM_GFX_CNTL need a GRBM or SRBM select (a register write), which
//          this read-only query does not make
//   [28..31] reserved
// A driver that predates the tag declines with HSA_STATUS_ERROR_INVALID_ARGUMENT.
typedef struct mac_hsa_device_spec_s {
    uint32_t words[32];
} mac_hsa_device_spec_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_get_device_spec(hsa_agent_t agent,
                                           mac_hsa_device_spec_t *spec,
                                           size_t spec_size);

// Raw CP dispatch timestamps in this GPU's clock domain, NOT HSA system time.
// Enable queue profiling before its first submission. Use a fresh completion
// signal per dispatch, SYSTEM release scope, and retain it unchanged until readout.
// The caller associates the signal with its submitting queue; this API does not
// prove that association. No submission/wait is performed; pending/missing stamps
// fail without changing output. Cross-device/SDMA/host clock correlation is absent.
enum { MAC_HSA_TIMESTAMP_DOMAIN_GPU = 1 };
typedef struct mac_hsa_dispatch_timestamps_s {
    uint64_t version, start_ticks, end_ticks, frequency_hz;
    uint32_t valid_bits, clock_domain;
} mac_hsa_dispatch_timestamps_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_dispatch_timestamps(const hsa_queue_t *queue,hsa_signal_t completion,
    mac_hsa_dispatch_timestamps_t *out,size_t out_size);

// Synchronous native launch of a frozen HSA-loaded kernel (any code object the
// agent's ISA accepts). This is not an HSA AQL queue. The native PM4 ABI
// currently accepts wave32 only (GFX10 and later), a kernarg-pointer-only user
// SGPR layout, and no scratch, LDS, preload or dynamic stack. The caller lists
// every device/shared allocation referenced by kernargs; ordinary unmapped CPU
// pointers are invalid. The driver must be build 183 or newer.
// Buffers/code remain retained through completion. A failed GPU launch requires
// session recovery. Group counts are workgroups, not individual workitems.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_executable_dispatch(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t kernarg_size, const uint32_t groups[3],
    const uint32_t threads[3], const void *const *buffers, size_t buffer_count,
    uint64_t *completion_fence);

// Bounded hardware AQL dispatch (driver 184+), with the kernel limits of the
// native PM4 path above, except that the packet processor builds the user
// SGPRs from the descriptor: wave64 kernels and the private segment buffer
// SGPRs of targets without architected flat scratch are accepted. Success proves a GPU-only VRAM completion signal
// changed from 1 to 0 and firmware acknowledged queue removal. This does not
// expose a persistent hsa_queue_t or CPU/GPU atomic signals. Output is the
// observed signal value (zero on success), not a monotonic fence sequence.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_executable_dispatch_aql(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t kernarg_size, const uint32_t groups[3],
    const uint32_t threads[3], const void *const *buffers, size_t buffer_count,
    uint64_t *completion);

// Explicit coarse shared allocation for the native launch path. CPU and GPU
// addresses are identical. CPU access is allowed only between completed GPU
// operations; this does not provide system atomics or HSA fine-grained memory.
// Release with hsa_memory_free; normal HSA pool capabilities are unchanged.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_memory_allocate_shared(hsa_agent_t agent, size_t size, void **out);

// Caps the host memory the runtime shares with the GPU at once: every shared
// (GTT) buffer together, whatever it holds (staging, kernel arguments,
// signals, AQL rings). An allocation past the cap fails with
// HSA_STATUS_ERROR_OUT_OF_RESOURCES and a message on stderr. Zero removes
// the cap (the default; the host window is then the only limit). Overrides
// MAC_HSA_HOST_MEMORY_BUDGET (bytes, optional K/M/G suffix). Call it before
// hsa_init to also size the host window the driver is asked for.
__attribute__((visibility("default")))
void mac_hsa_set_host_memory_budget(uint64_t bytes);

// Stores into the GPU's BARs on a client's submission path, as HRX makes them
// on Linux: kernel arguments written into CPU-visible VRAM, the HDP flush
// register stored and read back, the doorbell. On this platform a CPU store
// to a GPU that stopped answering (it powered down, reset, or left the
// Thunderbolt link) can panic the Mac, so a client that makes them opts in
// and brackets every submission's stores:
//
//   mac_hsa_bar_writes_enable(gpu, &writer);   // once, before querying the agent
//   ...
//   if (mac_hsa_bar_write_begin(writer) == HSA_STATUS_SUCCESS) {
//       ... memcpy into the kernarg ring, HDP store + read back, doorbell ...
//       mac_hsa_bar_write_end(writer);
//   }
//
// Only after enable does the agent report HSA_AMD_AGENT_INFO_HDP_FLUSH and
// the CPU agent's access to the GPU's coarse VRAM pool as
// HSA_AMD_MEMORY_POOL_ACCESS_DISALLOWED_BY_DEFAULT; hsa_amd_agents_allow_access
// with a CPU agent then maps a pool allocation for the CPU (write combined,
// at its own address, pinned in the CPU-visible VRAM window), as ROCr does
// on a large-BAR device. Enable fails, saying why on stderr, with a driver
// older than build 265 or a session that is not a KFD process; nothing is
// enabled then. begin waits while the driver holds the gate for a power
// transition or a device reset; once the device is gone for this process it
// retires every mapping of the GPU (later stores land in host memory) and
// returns kDeviceLostStatus (HSA_STATUS_ERROR_FATAL) without opening a
// bracket: do not call end then. Brackets nest; the runtime's own doorbell
// is a nested one. Keep them short: a driver close waits for open brackets
// (up to a bound), so a bracket should not wait on the GPU or block.
typedef struct mac_hsa_bar_writer_s mac_hsa_bar_writer_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_bar_writes_enable(hsa_agent_t agent, mac_hsa_bar_writer_t **writer);
__attribute__((visibility("default")))
hsa_status_t mac_hsa_bar_write_begin(mac_hsa_bar_writer_t *writer);
__attribute__((visibility("default")))
void mac_hsa_bar_write_end(mac_hsa_bar_writer_t *writer);

// What every GPU this runtime opened holds from its driver, and where in the
// process each buffer was asked for: buffer counts and bytes (VRAM and
// shared), a VRAM size histogram, and the callers holding the most VRAM with
// their return addresses. Text, one fact per line. Writes at most `capacity`
// bytes including the terminating NUL and returns the length of the whole
// report, so a short buffer can be retried with the size returned. 0 before
// hsa_init. MAC_HSA_VRAM_TRACE=1 also logs every buffer as it is allocated
// and freed.
__attribute__((visibility("default")))
size_t mac_hsa_memory_report(char *buffer, size_t capacity);

enum {
    MAC_HSA_SYNC_CPU_LOCAL_ATOMICS = 1u << 0,
    MAC_HSA_SYNC_GPU_LOCAL_ATOMICS = 1u << 1,
    MAC_HSA_SYNC_OWNERSHIP_TRANSFER = 1u << 2,
    MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS = 1u << 3,
    MAC_HSA_SYNC_NATIVE_CPU_GPU_RMW = 1u << 4,
    MAC_HSA_SYNC_MAILBOX_IRQ_WAKE = 1u << 5
};
// Query a tracked allocation and its actual GPU mapping path. LOCAL flags allow
// atomics within one agent domain, not simultaneous CPU/GPU RMW on one word.
// OWNERSHIP_TRANSFER permits the tested release/acquire ownership protocol; it
// does not make the pool fine-grained. GPU_MEDIATED_SIGNALS describes HSA signal
// API routing, not automatic interception of arbitrary pointer atomics. Neither
// native mixed RMW nor mailbox IRQ wake is qualified by the current profiles.
// No device initialization, config writes, or submission. Output unchanged on
// failure; pass its GPU agent for GPU allocations, CPU agent for host-only ones.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_memory_get_sync_capabilities(hsa_agent_t agent,
    const void *pointer, uint32_t *flags);

// Driver190+: read-only snapshot for the aligned 64-bit word at shared_pointer
// and an owned live hardware queue on the same GPU. This does not submit work,
// initialize the device or change mappings/PCIe/MQD policy. Caller keeps the
// queue idle while sampling; MQD backing is NOT a live selected HQD register.
// valid_fields bits: 1=PTE, 2=MQD backing, 4=PCIe endpoint, 8=GFXHUB context.
// CPU map options are the requested policy; actual CPU cache/MAIR attributes
// have no public query and remain UINT64_MAX. Output is unchanged on failure.
typedef struct mac_hsa_shared_atomic_diagnostics_s {
    uint64_t version, valid_fields, gpu_address, dma_address, pte_vram_offset;
    uint64_t pte_actual, pte_expected, mqd_gpu_address, mqd_backing_hq_status0;
    uint64_t pcie_capability_offset, pcie_device_capabilities2, pcie_device_control2;
    uint64_t gfxhub_page_table_base, gfxhub_context0_control;
    uint64_t cpu_mapping_options, cpu_cache_attributes;
} mac_hsa_shared_atomic_diagnostics_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_shared_atomic_diagnostics(const void *shared_pointer,
    const hsa_queue_t *queue, mac_hsa_shared_atomic_diagnostics_t *out, size_t out_size);

// Driver191 explicit experiment on an unqualified PCIe path. enable=1 sets
// only endpoint DeviceControl2 bit6; enable=0 restores its original value.
// No queue/submission may remain at either transition. Requires exclusive
// ownership; does not change HSA capabilities, PTE/cache/MQD policy or memory.
// A valid driver reply is copied even when its operation failed: inspect
// driver_status/restore_pending. Transport/argument failures leave out unchanged.
// Hot unplug or driver-process failure cannot guarantee software restoration.
typedef struct mac_hsa_atomic_requester_experiment_s {
    uint64_t version, before_control2, requested_control2, observed_control2;
    uint64_t original_control2, active, restore_pending, driver_status;
} mac_hsa_atomic_requester_experiment_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_atomic_requester_experiment(hsa_agent_t agent,uint32_t enable,
    mac_hsa_atomic_requester_experiment_t *out,size_t out_size);

#ifdef __cplusplus
}
#endif
