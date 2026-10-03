/* KFD compute sessions: one Linux process per runtime client, driving the
 * unmodified upstream KFD through its own character device.
 *
 * A session is what a ROCr/libhsakmt process is on Linux. Opening one
 * creates a linuxu process (task, mm, files), opens the KFD chardev and the
 * device's DRM render node into that process's descriptor table, and then
 * issues the ioctls libhsakmt issues when it opens KFD (hsaKmtOpenKFD and
 * hsakmt_fmm_init_process_apertures): GET_VERSION,
 * GET_PROCESS_APERTURES_NEW, ACQUIRE_VM(render fd), SET_MEMORY_POLICY and
 * RUNTIME_ENABLE. Every call runs inside the process (linuxu_process_enter)
 * through the registered file_operations, with its argument block in a
 * per-call user VMA, so KFD's copy_from_user/copy_to_user see what they see
 * on Linux.
 *
 * Memory is ALLOC_MEMORY_OF_GPU + MAP_MEMORY_TO_GPU at a GPU VA in the
 * process's GPUVM (its own VMID while queues run). Queues are CREATE_QUEUE
 * (COMPUTE_AQL) with ring, read/write pointers, EOP and context-save areas
 * that are BOs of the same process, as kfd_queue_acquire_buffers requires;
 * KFD's device queue manager hands them to the MES scheduler. Kicks write
 * the doorbell KFD allocated, located through KFD's own doorbell mmap.
 *
 * Closing destroys every queue and BO, then exits the process: the mmu
 * notifier release runs KFD's process teardown (exit_mm) and the KFD and
 * render descriptors are closed (exit_files), as when a Linux process dies.
 *
 * All calls on one session are serialized by the session. A session never
 * hard-codes a GPU: sizes come from the KFD topology node and apertures
 * from the ioctls. Runtime: linuxu/src/amdgpu-rt/kfd_session.c. */
#ifndef LINUXU_RT_KFD_SESSION_H
#define LINUXU_RT_KFD_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amdgpu_device;
struct rt_compute_ctx;
struct rt_kfd_session;
struct rt_kfd_bo;
struct rt_kfd_queue;

/* 0 when @adev can back a session: KFD bound a device node to it (a GPU
 * node in the KFD topology), its queue manager schedules through MES, and
 * the KFD and DRM character devices are registered. -ENODEV otherwise. */
int rt_kfd_session_supported(struct amdgpu_device *adev);

/* The process-device apertures KFD reported for this device
 * (GET_PROCESS_APERTURES_NEW), plus the KFD interface version. */
struct rt_kfd_apertures {
	uint32_t gpu_id;
	uint32_t version_major, version_minor;
	uint64_t lds_base, lds_limit;
	uint64_t scratch_base, scratch_limit;
	uint64_t gpuvm_base, gpuvm_limit;
};

/* Queue resources KFD's topology node prescribes for one compute queue.
 * slots is the number of queues a process may create on the device
 * (pqm_create_queue's per-process limit). */
struct rt_kfd_queue_limits {
	uint32_t slots;
	uint32_t eop_bytes;
	uint32_t ctx_save_bytes;	/* per XCC (node cwsr_size) */
	uint32_t ctl_stack_bytes;
	uint32_t debug_bytes;		/* per XCC (node debug_memory_size) */
	uint32_t xcc_count;
	uint64_t ctx_area_bytes;	/* what CREATE_QUEUE validates */
	uint32_t doorbell_slice_bytes;
};

/* Open a session for a client whose pid is @pid (a counter value when
 * pid <= 0) and whose command name is @comm. @ctx supplies the GART-mapped
 * staging the session copies VRAM through. */
int rt_kfd_session_open(struct amdgpu_device *adev, struct rt_compute_ctx *ctx,
			int pid, const char *comm, struct rt_kfd_session **out);
/* Destroy every queue (DESTROY_QUEUE), free every BO (UNMAP + FREE), exit
 * the process and free @s. Returns 0 after a clean close. A queue whose
 * removal failed leaves the GPU possibly using the session's memory: the
 * session is then kept (returns -EBUSY), every later call fails, and the
 * caller must treat the device as uncertain. */
int rt_kfd_session_close(struct rt_kfd_session *s);
/* Nonzero once a teardown step failed (see rt_kfd_session_close). */
int rt_kfd_session_uncertain(const struct rt_kfd_session *s);

int rt_kfd_session_pid(const struct rt_kfd_session *s);
int rt_kfd_session_apertures(struct rt_kfd_session *s, struct rt_kfd_apertures *out);
int rt_kfd_session_queue_limits(struct rt_kfd_session *s, struct rt_kfd_queue_limits *out);

/* The client's host window: the range of process VAs the client maps its
 * shared buffers at (CPU VA == GPU VA). The session offers a power-of-two
 * size covering the host memory a process can map
 * (rt_kfd_session_window_size); the client reserves that much of its own
 * address space, aligned to the size, and sets the base once, before any
 * window BO exists (@size 0: the offered size; a smaller power of two keeps
 * a window the client already reserved, such as the GART window). The
 * window must lie inside the GPUVM aperture and below the session's private
 * range (the top quarter of the aperture). rt_kfd_session_window reports a
 * zero base until it is set, and the offered size until then. */
uint64_t rt_kfd_session_window_size(struct rt_kfd_session *s);
int rt_kfd_session_set_window(struct rt_kfd_session *s, uint64_t base, uint64_t size);
int rt_kfd_session_window(struct rt_kfd_session *s, uint64_t *base, uint64_t *size);

enum rt_kfd_domain {
	RT_KFD_GTT = 1,
	RT_KFD_VRAM = 2,
};
/* Placement of a BO's VA. WINDOW: inside the client's host window (it is
 * mapped by the client at that VA). PRIVATE: in the session's private range
 * (GPU-only memory the client never maps: queue EOP/context save, scratch,
 * VRAM). */
enum rt_kfd_place {
	RT_KFD_PLACE_PRIVATE = 0,
	RT_KFD_PLACE_WINDOW = 1,
};
struct rt_kfd_bo_info {
	uint64_t va;
	uint64_t size;
	uint64_t handle;	/* the KFD handle (gpu_id << 32 | idr) */
	enum rt_kfd_domain domain;
};
/* ALLOC_MEMORY_OF_GPU at a VA the session chooses in @place, then
 * MAP_MEMORY_TO_GPU on the session's device. @alignment is a power of two
 * (0: the page size). GTT is coherent host memory; VRAM is device-local and
 * never CPU-mapped (small BAR). */
int rt_kfd_bo_alloc(struct rt_kfd_session *s, uint64_t size, uint64_t alignment,
		    enum rt_kfd_domain domain, enum rt_kfd_place place,
		    struct rt_kfd_bo **out);
/* UNMAP_MEMORY_FROM_GPU + FREE_MEMORY_OF_GPU. -EBUSY while a queue of the
 * session uses the BO. */
int rt_kfd_bo_free(struct rt_kfd_session *s, struct rt_kfd_bo *bo);
int rt_kfd_bo_info(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		   struct rt_kfd_bo_info *out);
/* Kernel-side access: GTT through the BO's pages, VRAM through SDMA copies
 * between the BO's VRAM ranges and the session's staging. */
int rt_kfd_bo_read(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		   uint64_t offset, void *dst, size_t bytes);
int rt_kfd_bo_write(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		    uint64_t offset, const void *src, size_t bytes);
int rt_kfd_bo_copy(struct rt_kfd_session *s, struct rt_kfd_bo *src,
		   uint64_t src_offset, struct rt_kfd_bo *dst,
		   uint64_t dst_offset, uint64_t bytes);
/* Walk the host pages backing a GTT BO as maximal CPU-contiguous runs, in
 * BO order; @fn returning nonzero stops the walk with that value. The pages
 * stay valid until rt_kfd_bo_free. -EINVAL for VRAM. */
int rt_kfd_bo_cpu_ranges(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
			 int (*fn)(void *arg, void *cpu, uint64_t bytes), void *arg);

/* One AQL compute queue. ring, read/write pointer addresses are VAs inside
 * BOs of this session: the ring BO must be exactly PAGE_ALIGN(ring_bytes)
 * and the pointers inside the first GPU page of a one-page BO (KFD checks
 * both). The session allocates the EOP buffer (VRAM) and the context-save
 * area (GTT, header initialized as libhsakmt does) for the queue. */
struct rt_kfd_queue_desc {
	struct rt_kfd_bo *ring;
	uint32_t ring_bytes;
	uint64_t read_pointer;	/* VA */
	uint64_t write_pointer;	/* VA */
	uint32_t priority;	/* KFD queue priority, 0..15 */
};
struct rt_kfd_queue_info {
	uint32_t queue_id;
	uint32_t doorbell_index;	/* dword index on the doorbell BAR */
	uint64_t doorbell_offset;	/* CREATE_QUEUE's mmap offset + byte offset */
	uint64_t eop_va, ctx_save_va;
};
int rt_kfd_queue_create(struct rt_kfd_session *s, const struct rt_kfd_queue_desc *desc,
			struct rt_kfd_queue **out);
int rt_kfd_queue_info(struct rt_kfd_session *s, struct rt_kfd_queue *q,
		      struct rt_kfd_queue_info *out);
/* Write @value to the queue's doorbell (64-bit doorbells on SOC15). */
int rt_kfd_queue_kick(struct rt_kfd_session *s, struct rt_kfd_queue *q, uint64_t value);
/* DESTROY_QUEUE, then free the queue's EOP and context-save BOs. A failed
 * DESTROY_QUEUE makes the session uncertain. */
int rt_kfd_queue_destroy(struct rt_kfd_session *s, struct rt_kfd_queue *q);
unsigned int rt_kfd_session_queue_count(struct rt_kfd_session *s);

#ifdef __cplusplus
}
#endif
#endif
