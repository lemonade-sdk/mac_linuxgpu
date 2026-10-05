/* linuxu shim: dext_compute.h — the COMPUTE selector seam C API.
 *
 * T-dext-userclient-seam: the userspace HSA runtime (hsa/ libhsa-runtime64.dylib)
 * talks to the dext over the IOKit user-client selector RPC.  The compute
 * selectors (RuntimeBuild=43, QueryInfo=21, BO*=16-18/36, AQLQueue*=56-59,
 * AQLDispatch=55, HostWindow=54, ShutdownGPU=42, GetReBARInfo=41,
 * HostMemoryTest=44, WaitFence=20, SubmitIB=19, CS*=37-39) all route to the
 * functions declared here.  The selector bodies in MacLinuxGPUXcode.mm read
 * their IOUserClientMethodArguments in the reference's layout and call these.
 *
 * The DriverKit build uses upstream AMDGPU compute objects and real queue
 * operations. The host build retains the in-memory selector model.
 *
 * This header is included by: the selector bodies (dext-only), the dext I/O
 * hook impl (dext-only), and the host unit test (host-only).  It must be
 * self-contained (no DriverKit headers).
 */
#ifndef LINUXU_DEXT_COMPUTE_H
#define LINUXU_DEXT_COMPUTE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (the linuxu convention) ---- */
#define ENOTREADY_L   11001
#define EAGAIN_L      11002
#define ENOENT_L      11003
#define EINVAL_L      11004
#define ENOMEM_L      11005
#define EBUSY_L       11006

/* QueryInfo diagnostic namespace: attempted, bound, signed probe result,
 * transport fault, and transport fault offset. */
#define DEXT_COMPUTE_QUERY_PROBE_STATUS 0x4c50524fULL
#define DEXT_COMPUTE_QUERY_KERNEL_LOG   0x4c4c4f47ULL
/* Cached session lifecycle and quarantine cause; layout in session_state.h. */
#define DEXT_COMPUTE_QUERY_SESSION_STATE 0x4c534553ULL

/* ---- bringup stage (mirrors amdgpu::BringupStage) ---- */
enum dext_compute_stage {
    DEXT_COMPUTE_STAGE_NONE      = 0,
    DEXT_COMPUTE_STAGE_GFX_INIT  = 1,
    DEXT_COMPUTE_STAGE_SDMA_INIT = 2,   /* the reference's "ready" stage */
    DEXT_COMPUTE_STAGE_COMPUTE   = 3,
};

/* ---- BO domains (match the reference's kBODomain*) ---- */
enum dext_compute_bo_domain {
    DEXT_COMPUTE_BO_DOMAIN_GTT_LEGACY  = 0,
    DEXT_COMPUTE_BO_DOMAIN_VRAM        = 1,
    DEXT_COMPUTE_BO_DOMAIN_GTT         = 2,
    DEXT_COMPUTE_BO_DOMAIN_DEVICE_VRAM = 3,
};

/* ---- CS IP types (match the reference's kMacAMDGPUCSIPType*) ---- */
enum dext_compute_cs_ip {
    DEXT_COMPUTE_CS_IP_SDMA    = 0,
    DEXT_COMPUTE_CS_IP_GFX     = 1,
    DEXT_COMPUTE_CS_IP_COMPUTE = 2,
};

/* ---- the device properties snapshot (QueryInfo tag 6) ---- */
struct dext_compute_props {
    uint64_t chip_id;
    uint64_t revision;
    uint64_t bdf;
    uint64_t domain;
    uint64_t compute_units;
    uint64_t shader_engines;
    uint64_t arrays_per_engine;
    uint64_t timestamp_frequency;
    uint64_t max_waves_per_cu;
    uint64_t wavefront_size;
    bool     valid;
};

/* ---- the optional dext I/O hook (dext_compute_dk.mm provides it under
 * LINUXU_DEXT).  op is the opcode; in/out are the per-op payloads defined by
 * dext_compute.c.  The host build leaves the hook NULL. */
typedef bool (*dext_compute_open_hook)(void);
typedef int  (*dext_compute_gpu_op_hook)(int op, const void *in, void *out,
                                         size_t out_size);

/* ---- state setters (the dext calls these after open/cold-boot) ---- */
void dext_compute_set_hooks(dext_compute_open_hook open_hook,
                            dext_compute_gpu_op_hook gpu_op_hook);
void dext_compute_set_pci_open(bool open);
void dext_compute_set_stage(enum dext_compute_stage stage);
void dext_compute_set_vram(uint64_t base, uint64_t visible, uint64_t real);
void dext_compute_set_gfx_version(uint32_t major, uint32_t minor,
                                  uint32_t revision);
void dext_compute_set_props(const struct dext_compute_props *props);
void dext_compute_set_claimed(bool claimed);
void dext_compute_reset(void);

/* ---- the compute seam C API (one function per compute selector) ---- */
struct pci_dev;
int dext_compute_start(struct pci_dev *pdev);
/* The step and error of the last failed dext_compute_start (NULL, 0 if none). */
void dext_compute_start_failure(const char **step, int *error);
int dext_compute_stop(void);
/* The device left the bus (surprise removal, rt/removal.h): nothing the
 * runtime kept for GPU work that might still run needs keeping any more,
 * so stopping and client release no longer refuse on that account. */
void dext_compute_device_removed(void);
/* Serialized by the driver lifecycle queue. Handles and mapped memory types
 * belong to the selected client; client zero is reserved for internal use. */
uint64_t dext_compute_select_client(uint64_t client);
int dext_compute_release_client(uint64_t client);
int dext_compute_bo_export(uint64_t handle, uint64_t first, uint64_t second,
                            uint64_t out[3]);
int dext_compute_bo_import(uint64_t first, uint64_t second, uint64_t size,
                            uint64_t out[3]);

/* AtomicRequester (60): in enable (0 or 1); out the 8-word
 * amdgpu::atomic_requester::Snapshot (version, before, requested, observed,
 * original, active, restore_pending, status). PCIe AtomicOps cannot be
 * routed to the root port over TB5 (pci_enable_atomic_ops_to_root() reports
 * -EOPNOTSUPP), so no configuration write is attempted: enable reports
 * status DEXT_COMPUTE_ATOMIC_UNSUPPORTED and disable reports nothing to
 * restore. */
#define DEXT_COMPUTE_ATOMIC_REQUESTER_WORDS 8
#define DEXT_COMPUTE_ATOMIC_UNSUPPORTED     95 /* Linux EOPNOTSUPP */
int dext_compute_atomic_requester(uint64_t enable, uint64_t *out /* 8 */);

/* RuntimeBuild (43): out[0]=magic, out[1]=ABI, out[2]=build. */
int dext_compute_runtime_build(uint64_t *out /* 3 dwords */);
/* The same answer from cached lifecycle flags only (no upstream device
 * access), for observer clients; out[3] is the compiled build regardless of
 * readiness. */
int dext_compute_runtime_build_cached(uint64_t *out /* 4 dwords */);
/* No compute context and no uncertain GPU work remain. Cached state only. */
int dext_compute_quiescent(void);

/* QueryInfo (21): in tag, out values[].  Returns the number of out values
 * written on success, or a -E*_L error. */
int dext_compute_query_info(uint64_t tag, uint64_t *out, int out_cap);
/* QueryInfo tag 8 (session_state.h's struct mlg_device_spec): up to @cap
 * bytes of it in @out; the bytes filled, or a negative linuxu errno
 * (-ENOTREADY_L before the device is up, -EINVAL_L for room under the
 * header). */
int dext_compute_device_spec(void *out, size_t cap);
/* Whether @client uses the legacy path (its session ends with it: the GART
 * host window is in its address space). */
bool dext_compute_client_legacy(uint64_t client);

/* BOAlloc (16): in size/domain/alignment/flags, out handle/gpu_va/cpu_addr. */
int dext_compute_bo_alloc(uint64_t size, uint32_t domain, uint64_t alignment,
                          uint64_t flags, uint64_t *out_handle,
                          uint64_t *out_gpu_va, uint64_t *out_cpu_addr);

/* BOFree (17): in handle. */
int dext_compute_bo_free(uint64_t handle);

/* BOGetInfo (18): in handle, out up to 5 (gpu_va/byte_off/size/align/domain). */
int dext_compute_bo_get_info(uint64_t handle, uint64_t *out);

/* BOMap (36): in handle, out memory_type/size. */
int dext_compute_bo_map(uint64_t handle, uint64_t *out);
/* -EAGAIN_L (size set, cpu NULL) for a BO whose pages are not one
 * allocation (a KFD process's GTT BO): map dext_compute_bo_memory_ranges. */
int dext_compute_bo_memory(uint32_t memory_type, void **cpu, uint64_t *size);
int dext_compute_bo_memory_ranges(uint32_t memory_type,
                                  int (*fn)(void *arg, void *cpu, uint64_t bytes),
                                  void *arg);
/* Compute sessions. Policy: a client becomes a KFD process (its own GPUVM,
 * MES user queues) when the device supports it, unless disabled here; the
 * legacy VMID0 path otherwise. Identity: the pid and command name the
 * client's KFD process gets (set before the client's first compute call). */
void dext_compute_set_kfd_policy(bool enabled);
int dext_compute_client_identity(uint64_t client, int pid, const char *comm);
int dext_compute_bo_copy(uint64_t source, uint64_t destination,
                         uint64_t source_offset, uint64_t destination_offset,
                         uint32_t bytes);
int dext_compute_bo_write(uint64_t handle, uint64_t offset,
                          const void *source, size_t bytes);
int dext_compute_bo_read(uint64_t handle, uint64_t offset,
                         void *destination, size_t bytes);
int dext_compute_dispatch(const void *request, size_t request_size,
                          uint64_t *out /* 3 */);
/* ComputeDispatch (51) validation: whether the device's COMPUTE_PGM_RSRC1
 * has DX10_CLAMP/IEEE_MODE (compute_dispatch_shape()'s second argument). */
int dext_compute_rsrc1_clamp_ieee(void);

/* AQLQueueCreate (56): in ring/metadata/packets, out status/handle. */
int dext_compute_aql_queue_create(uint64_t ring_handle,
                                  uint64_t metadata_handle,
                                  uint64_t packets,
                                  uint64_t *out_status,
                                  uint64_t *out_handle);

/* AQLQueueKick (57): in handle/wptr, out status. */
int dext_compute_aql_queue_kick(uint64_t handle, uint64_t wptr,
                                uint64_t *out_status);

/* AQLQueueDestroy (58): in handle, out status. */
int dext_compute_aql_queue_destroy(uint64_t handle, uint64_t *out_status);

/* AQLQueueService (59): in handle, out status/inactive. */
int dext_compute_aql_queue_service(uint64_t handle, uint64_t *out_status,
                                   uint64_t *out_inactive);

/* AQLDispatch (55): in the AQLDispatchRequest struct, out 5 scalars. */
int dext_compute_aql_dispatch(const void *request, size_t request_size,
                              uint64_t *out /* 5 */);

/* HostWindow (54): in configure(0/1), out gart_start/gart_size/reads. */
int dext_compute_host_window(uint64_t configure, uint64_t *out /* 3 */);

/* ShutdownGPU (42): out status/phase (phase 6 = complete). */
int dext_compute_shutdown(uint64_t *out /* 2 */);

/* GetReBARInfo (41): in bar, out offset/cap/ctl/supported/selected/assigned. */
int dext_compute_get_rebar(uint64_t bar, uint64_t *out /* 6 */);

/* HostMemoryTest (44): in size, out 6 scalars. */
int dext_compute_host_mem_test(uint64_t size, uint64_t *out /* 6 */);

/* WaitFence (20): in fence_handle/timeout_ns, out status (0=signaled,1=timeout). */
int dext_compute_wait_fence(uint64_t fence_handle, uint64_t timeout_ns,
                            uint64_t *out_status);

/* SubmitIB (19): in cs_handle, out fence_handle (== cs_handle). */
int dext_compute_submit_ib(uint64_t cs_handle, uint64_t *out_fence);

/* CSCreate (37): in ip_type/ip_instance, out handle. */
int dext_compute_cs_create(uint32_t ip_type, uint32_t ip_instance,
                           uint64_t *out_handle);

/* CSWriteDwords (38): in handle/dwords/count, out written. */
int dext_compute_cs_write_dwords(uint64_t handle, const uint32_t *dwords,
                                 uint32_t count, uint32_t *out_written);

/* CSDestroy (39): in handle. */
int dext_compute_cs_destroy(uint64_t handle);

/* Signal events (selector 86) of the selected client's KFD process: the
 * runtime's interrupt signals. create: out[0] id, [1] trigger (the
 * amd_signal_t event_id), [2] mailbox VA (event_mailbox_ptr). A client on
 * the legacy path has none: -ENOTREADY_L. */
#define DEXT_COMPUTE_EVENT_CREATE  0u
#define DEXT_COMPUTE_EVENT_DESTROY 1u
#define DEXT_COMPUTE_EVENT_SET     2u
int dext_compute_event(uint32_t op, uint32_t id, uint64_t out[3]);
/* Selector 87: register a wait on the selected client's events
 * (rt_kfd_wait_begin); the caller runs it with rt_kfd_wait_run off its
 * queue, or frees it with rt_kfd_wait_cancel. -EBUSY_L when the client
 * has too many waits running. */
struct rt_kfd_wait;
int dext_compute_event_wait_begin(const uint32_t *ids, uint32_t count, int all,
                                  uint32_t timeout_ms, struct rt_kfd_wait **out);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_DEXT_COMPUTE_H */
