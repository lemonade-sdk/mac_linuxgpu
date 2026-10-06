/* KFD compute sessions (rt/kfd_session.h): a linuxu process per runtime
 * client, driving the unmodified upstream KFD through /dev/kfd's
 * file_operations exactly as libhsakmt does on Linux. */
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysinfo.h>
#include <linux/dma-fence.h>
#include <linux/jiffies.h>
#include <uapi/linux/kfd_ioctl.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/ttm/ttm_tt.h>
#include <rt/chrdev.h>
#include <rt/compute.h>
#include <rt/kfd_session.h>
#include <rt/process.h>
#include <rt/process_file.h>
#include <rt/removal.h>

#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "amdgpu_object.h"
#include "amdgpu_res_cursor.h"
#include "amdgpu_reset.h"
#include "amdgpu_ttm.h"
#include "kfd_priv.h"
#include "kfd_events.h"
#include "kfd_device_queue_manager.h"
#include "kfd_topology.h"

/* CPU-side VAs the session's own mappings use: argument blocks and the
 * doorbell mmap. They lie above every GPUVM aperture (gpuvm_limit < 2^47)
 * and below TASK_SIZE_MAX, so no GPU VA and no client mapping overlaps. */
#define RT_KFD_ARENA_VA		(1ULL << 47)
#define RT_KFD_DOORBELL_VA	(RT_KFD_ARENA_VA + (1ULL << 40))
/* Each running wait's argument block: its own VA (rt_process_ioctl maps
 * the block there for the call), between the arena and the doorbells. */
#define RT_KFD_WAIT_VA		(RT_KFD_ARENA_VA + (1ULL << 39))
#define RT_KFD_WAIT_STRIDE	(1ULL << 20)
#define RT_KFD_EVENT_PAGE_BYTES	((uint64_t)KFD_SIGNAL_EVENT_LIMIT * 8)
/* GART-mapped staging for SDMA transfers to and from VRAM. */
#define RT_KFD_STAGING_BYTES	(1ULL << 20)
#define RT_KFD_LARGE_PAGE	(2ULL << 20)
/* libhsakmt's priority_map[] entry for HSA_QUEUE_PRIORITY_NORMAL. */
#define RT_KFD_PRIORITY_NORMAL	7
#ifndef RT_KFD_COPY_TIMEOUT_MS
#define RT_KFD_COPY_TIMEOUT_MS	5000
#endif
/* How long a session about to close waits for a copy that outlived its
 * timeout before it keeps the memory the copy may still touch. */
#ifndef RT_KFD_SETTLE_MS
#define RT_KFD_SETTLE_MS	RT_KFD_COPY_TIMEOUT_MS
#endif

struct va_range {
	struct va_range *next;
	uint64_t start, end;
};
struct va_region {
	uint64_t base, end;	/* [base, end) */
	struct va_range *head;	/* sorted by start */
};

struct rt_kfd_bo {
	struct rt_kfd_bo *next;
	struct kgd_mem *mem;
	struct amdgpu_bo *abo;
	uint64_t handle;
	uint64_t va, size;
	enum rt_kfd_domain domain;
	enum rt_kfd_place place;
	unsigned int queue_uses;
};

struct rt_kfd_queue {
	struct rt_kfd_queue *next;
	struct rt_kfd_bo *ring, *eop, *ctx;
	struct rt_kfd_bo *pointers[2];	/* BOs holding rptr/wptr, besides the ring */
	uint32_t queue_id;
	uint32_t doorbell_index;
	uint64_t doorbell_offset;
	/* What MES knows the queue by (remove_queue_mes's input), read from
	 * KFD's queue when it was created. */
	uint32_t mes_doorbell;
	uint64_t mes_gang_ctx;
	bool mes_known;
	/* DESTROY_QUEUE failed: MES may still run the queue. */
	bool failed;
	/* ... after KFD had already let go of it (destroy_queue_cpsch takes
	 * the queue off its lists before MES confirms the removal). Only MES
	 * itself can finish such a queue; DESTROY_QUEUE must not run again. */
	bool detached;
	/* Off the GPU (destroyed, or recovered by a settle) but still linked
	 * until its owner destroys it or the session closes. */
	bool gone;
};

struct rt_kfd_session {
	struct amdgpu_device *adev;
	struct rt_compute_ctx *ctx;
	struct linuxu_process *proc;
	struct kfd_process *process;	/* owned by the KFD descriptor */
	int kfd_fd, drm_fd;
	struct rt_kfd_apertures ap;
	struct rt_kfd_queue_limits limits;
	pthread_mutex_t lock;
	struct rt_kfd_bo *bos;
	struct rt_kfd_queue *queues;
	unsigned int queue_count;
	struct va_region window, private_range;
	uint64_t window_size;
	bool window_set;
	bool doorbell_mapped;
	uint32_t doorbell_first;	/* dword index of the process slice */
	struct rt_compute_bo *staging;
	void *staging_cpu;
	uint64_t staging_mc;
	/* A copy that outlived RT_KFD_COPY_TIMEOUT_MS: the staging and the BO
	 * stay retained until its fence signals. */
	struct dma_fence *pending_copy;
	/* Something the GPU may still be using: a pending copy or a queue MES
	 * did not confirm removing. Every call but teardown is refused. */
	bool uncertain;
	/* The first teardown step that could not confirm the GPU let go. */
	enum rt_kfd_step fail_step;
	int fail_error;
	/* Signal events (rt_kfd_event_create) and the waits on them. */
	struct rt_kfd_bo *event_page;
	unsigned long event_ids[KFD_SIGNAL_EVENT_LIMIT / BITS_PER_LONG];
	unsigned int events;
	uint64_t wait_slots;		/* bit per running wait's VA */
	unsigned int waits;
	pthread_cond_t waits_done;
	bool closing;
	/* The process's memory event (rt_kfd_session_fault) and, once KFD
	 * signaled it, what it reported. */
	uint32_t memory_event;
	struct kfd_event *memory_ev;	/* KFD's, alive until the process exits */
	bool faulted;
	struct rt_kfd_fault fault;
};

struct rt_kfd_wait {
	struct rt_kfd_session *s;
	unsigned int slot;
	uint32_t count, all, timeout_ms;
	uint32_t ids[RT_KFD_WAIT_EVENTS_MAX];
};

/* ---- VA allocation ---- */

static uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static int va_alloc(struct va_region *r, uint64_t size, uint64_t align, uint64_t *out)
{
	struct va_range **link = &r->head;
	uint64_t cur;

	if (!size || !align || (align & (align - 1)) || r->base > UINT64_MAX - align)
		return -EINVAL;
	cur = align_up(r->base, align);
	for (;;) {
		uint64_t limit = *link ? (*link)->start : r->end;

		if (cur <= limit && size <= limit - cur) {
			struct va_range *range = kzalloc(sizeof(*range), GFP_KERNEL);

			if (!range)
				return -ENOMEM;
			range->start = cur;
			range->end = cur + size;
			range->next = *link;
			*link = range;
			*out = cur;
			return 0;
		}
		if (!*link)
			return -ENOMEM;
		if ((*link)->end > UINT64_MAX - align)
			return -ENOMEM;
		cur = align_up((*link)->end, align);
		link = &(*link)->next;
	}
}

static void va_free(struct va_region *r, uint64_t start)
{
	for (struct va_range **link = &r->head; *link; link = &(*link)->next) {
		if ((*link)->start == start) {
			struct va_range *range = *link;

			*link = range->next;
			kfree(range);
			return;
		}
	}
}

static void va_clear(struct va_region *r)
{
	while (r->head) {
		struct va_range *range = r->head;

		r->head = range->next;
		kfree(range);
	}
}

/* ---- process entry and ioctls ---- */

static int session_enter(struct rt_kfd_session *s, struct linuxu_process_saved *saved)
{
	return linuxu_process_enter(s->proc, saved);
}

static struct file *session_kfd_file(struct rt_kfd_session *s)
{
	return fget(s->kfd_fd);
}

/* Issue @cmd on the KFD descriptor as the process. The argument block (and
 * whatever arrays the caller laid out after it, addressed from
 * RT_KFD_ARENA_VA) travels in a user VMA that exists for this call only;
 * kfd_ioctl copies it in and out with copy_{from,to}_user. Caller is inside
 * the process. */
static long session_ioctl(struct rt_kfd_session *s, unsigned int cmd,
			  void *buf, size_t bytes)
{
	return rt_process_ioctl(s->kfd_fd, cmd, RT_KFD_ARENA_VA, buf, bytes);
}

static uint64_t arena_address(const void *base, const void *field)
{
	return RT_KFD_ARENA_VA + (uint64_t)((const char *)field - (const char *)base);
}

/* ---- capability ---- */

static struct kfd_node *session_node(struct amdgpu_device *adev)
{
	struct kfd_dev *kfd = adev ? adev->kfd.dev : NULL;

	if (!kfd || !kfd->init_complete || !kfd->num_nodes || !kfd->nodes[0])
		return NULL;
	return kfd->nodes[0];
}

int rt_kfd_session_supported(struct amdgpu_device *adev)
{
	struct kfd_node *node = session_node(adev);
	const struct file_operations *fops;
	struct drm_minor *render;
	unsigned int major;

	if (!node || !node->dqm || !node->kfd->shared_resources.enable_mes ||
	    !adev->enable_mes || node->dqm->sched_policy == KFD_SCHED_POLICY_NO_HWS)
		return -ENODEV;
	if (!kfd_topology_device_by_id(node->id))
		return -ENODEV;
	if (linuxu_chrdev_find("kfd", &major) ||
	    linuxu_chrdev_lookup(MKDEV(major, 0), &fops))
		return -ENODEV;
	render = adev_to_drm(adev)->render;
	if (!render || linuxu_chrdev_lookup(MKDEV(DRM_MAJOR, render->index), &fops))
		return -ENODEV;
	return 0;
}

/* pqm_create_queue's per-process limit (its max_queues: the HWS limit,
 * raised on GFX 9.4.3/9.4.4/9.5.0). */
static uint32_t process_queue_limit(struct kfd_node *node)
{
	const uint32_t gc = KFD_GC_VERSION(node);

	if (gc == IP_VERSION(9, 4, 3) || gc == IP_VERSION(9, 4, 4) ||
	    gc == IP_VERSION(9, 5, 0))
		return 255;
	return 127;
}

static int read_limits(struct rt_kfd_session *s)
{
	struct kfd_topology_device *topo = kfd_topology_device_by_id(s->ap.gpu_id);
	struct kfd_node *node = kfd_device_by_id(s->ap.gpu_id);
	struct rt_kfd_queue_limits *l = &s->limits;
	uint64_t area;

	if (!topo || !node)
		return -ENODEV;
	memset(l, 0, sizeof(*l));
	l->slots = process_queue_limit(node);
	l->eop_bytes = topo->node_props.eop_buffer_size;
	l->ctx_save_bytes = topo->node_props.cwsr_size;
	l->ctl_stack_bytes = topo->node_props.ctl_stack_size;
	l->debug_bytes = topo->node_props.debug_memory_size;
	l->xcc_count = NUM_XCC(node->xcc_mask);
	if (!l->xcc_count)
		l->xcc_count = 1;
	area = ((uint64_t)l->ctx_save_bytes + l->debug_bytes) * l->xcc_count;
	l->ctx_area_bytes = PAGE_ALIGN(area);
	l->doorbell_slice_bytes = (uint32_t)kfd_doorbell_process_slice(node->kfd);
	if (!l->ctx_save_bytes || !l->ctl_stack_bytes || !l->ctx_area_bytes ||
	    !l->doorbell_slice_bytes)
		return -ENODEV;
	return 0;
}

/* The host window offered to the client covers all host memory the
 * process can map (a power of two, as the runtime's reservation requires),
 * and the private range takes the top quarter of the GPUVM aperture. */
static void plan_ranges(struct rt_kfd_session *s)
{
	const uint64_t top = s->ap.gpuvm_limit + 1;
	const uint64_t private_size = top >> 2;
	struct sysinfo info;
	uint64_t want = 4ULL << 30;

	s->private_range.base = top - private_size;
	s->private_range.end = top;
	memset(&info, 0, sizeof(info));
	if (!si_meminfo(&info) && info.totalram) {
		uint64_t ram = (uint64_t)info.totalram * (info.mem_unit ? info.mem_unit : 1);

		while (want < ram && want < (1ULL << 44))
			want <<= 1;
	}
	while (want > 1 && want > (s->private_range.base - s->ap.gpuvm_base) / 2)
		want >>= 1;
	s->window_size = want;
}

/* ---- open/close ---- */

static int session_handshake(struct rt_kfd_session *s)
{
	struct amdgpu_device *adev = s->adev;
	struct kfd_node *node = session_node(adev);
	struct kfd_ioctl_get_version_args version = {0};
	struct kfd_ioctl_acquire_vm_args acquire = {0};
	struct kfd_ioctl_set_memory_policy_args policy = {0};
	struct {
		struct kfd_ioctl_get_process_apertures_new_args args;
		struct kfd_process_device_apertures nodes[8];
	} apertures;
	struct file *file;
	unsigned int major;
	uint32_t count, i;
	long r;

	/* hsaKmtOpenKFD: open /dev/kfd (kfd_open creates the KFD process for
	 * this mm), then GET_VERSION. */
	r = linuxu_chrdev_find("kfd", &major);
	if (r)
		return (int)r;
	s->kfd_fd = rt_process_open_chrdev(MKDEV(major, 0), O_RDWR | O_CLOEXEC);
	if (s->kfd_fd < 0)
		return s->kfd_fd;
	file = session_kfd_file(s);
	if (!file)
		return -EBADF;
	s->process = file->private_data;
	fput(file);
	if (!s->process)
		return -ESRCH;
	r = session_ioctl(s, AMDKFD_IOC_GET_VERSION, &version, sizeof(version));
	if (r)
		return (int)r;
	s->ap.version_major = version.major_version;
	s->ap.version_minor = version.minor_version;

	/* hsakmt_fmm_init_process_apertures: the render node of each GPU,
	 * the process apertures, then ACQUIRE_VM on that render fd. */
	s->drm_fd = rt_process_open_chrdev(MKDEV(DRM_MAJOR, adev_to_drm(adev)->render->index),
				O_RDWR | O_CLOEXEC);
	if (s->drm_fd < 0)
		return s->drm_fd;
	memset(&apertures, 0, sizeof(apertures));
	r = session_ioctl(s, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &apertures.args,
			  sizeof(apertures.args));
	if (r)
		return (int)r;
	count = apertures.args.num_of_nodes;
	if (!count)
		return -ENODEV;
	if (count > ARRAY_SIZE(apertures.nodes))
		count = ARRAY_SIZE(apertures.nodes);
	memset(&apertures, 0, sizeof(apertures));
	apertures.args.num_of_nodes = count;
	apertures.args.kfd_process_device_apertures_ptr =
		arena_address(&apertures, apertures.nodes);
	r = session_ioctl(s, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &apertures,
			  sizeof(apertures));
	if (r)
		return (int)r;
	for (i = 0; i < apertures.args.num_of_nodes && i < count; ++i) {
		const struct kfd_process_device_apertures *pa = &apertures.nodes[i];

		if (pa->gpu_id != node->id)
			continue;
		s->ap.gpu_id = pa->gpu_id;
		s->ap.lds_base = pa->lds_base;
		s->ap.lds_limit = pa->lds_limit;
		s->ap.scratch_base = pa->scratch_base;
		s->ap.scratch_limit = pa->scratch_limit;
		s->ap.gpuvm_base = pa->gpuvm_base;
		s->ap.gpuvm_limit = pa->gpuvm_limit;
	}
	if (!s->ap.gpu_id || !s->ap.gpuvm_limit || s->ap.gpuvm_limit <= s->ap.gpuvm_base ||
	    s->ap.gpuvm_limit >= RT_KFD_ARENA_VA)
		return -ENODEV;

	acquire.gpu_id = s->ap.gpu_id;
	acquire.drm_fd = s->drm_fd;
	r = session_ioctl(s, AMDKFD_IOC_ACQUIRE_VM, &acquire, sizeof(acquire));
	if (r)
		return (int)r;

	/* As libhsakmt for a GPUVM in the canonical address space: cached
	 * (non-coherent) by default, coherent in the alternate aperture. The
	 * host window is the coherent region; families with APE1 use it, later
	 * ones ignore the aperture and only program SH_MEM_CONFIG/BASES. */
	policy.gpu_id = s->ap.gpu_id;
	policy.default_policy = KFD_IOC_CACHE_POLICY_NONCOHERENT;
	policy.alternate_policy = KFD_IOC_CACHE_POLICY_COHERENT;
	r = session_ioctl(s, AMDKFD_IOC_SET_MEMORY_POLICY, &policy, sizeof(policy));
	if (r)
		return (int)r;

	/* hsaKmtRuntimeEnable (ROCr calls it at start-up on KFD >= 1.13). The
	 * debugger trap temporaries stay off. A refusal is not fatal, as in
	 * libhsakmt. */
	if (s->ap.version_major > 1 ||
	    (s->ap.version_major == 1 && s->ap.version_minor >= 13)) {
		struct kfd_ioctl_runtime_enable_args runtime = {0};

		runtime.mode_mask = KFD_RUNTIME_ENABLE_MODE_ENABLE_MASK;
		(void)session_ioctl(s, AMDKFD_IOC_RUNTIME_ENABLE, &runtime, sizeof(runtime));
	}
	/* ROCr's memory-exception event: KFD signals it when the process's
	 * GPU work faults, after evicting its queues. */
	{
		struct kfd_ioctl_create_event_args event = {0};

		event.event_type = KFD_IOC_EVENT_MEMORY;
		event.auto_reset = 0;
		r = session_ioctl(s, AMDKFD_IOC_CREATE_EVENT, &event, sizeof(event));
		if (r)
			return (int)r;
		s->memory_event = event.event_id;
		/* The event object itself: the queue service checks for a fault
		 * at 1 kHz per queue, and its signaled flag answers without a
		 * call. KFD frees it only with the process (the session never
		 * destroys it). */
		mutex_lock(&s->process->event_mutex);
		s->memory_ev = idr_find(&s->process->event_idr, event.event_id);
		mutex_unlock(&s->process->event_mutex);
		if (!s->memory_ev)
			return -ENOENT;
	}
	r = read_limits(s);
	if (r)
		return (int)r;
	plan_ranges(s);
	return 0;
}

static void session_free(struct rt_kfd_session *s)
{
	dma_fence_put(s->pending_copy);
	va_clear(&s->window);
	va_clear(&s->private_range);
	pthread_cond_destroy(&s->waits_done);
	pthread_mutex_destroy(&s->lock);
	kfree(s);
}

int rt_kfd_session_open(struct amdgpu_device *adev, struct rt_compute_ctx *ctx,
			int pid, const char *comm, struct rt_kfd_session **out)
{
	struct linuxu_process_saved saved;
	struct rt_kfd_session *s;
	struct rt_compute_bo_info info;
	int r;

	if (!out || !ctx)
		return -EINVAL;
	*out = NULL;
	r = rt_kfd_session_supported(adev);
	if (r)
		return r;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->adev = adev;
	s->ctx = ctx;
	s->kfd_fd = s->drm_fd = -1;
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->waits_done, NULL);
	r = rt_compute_bo_alloc(ctx, RT_KFD_STAGING_BYTES, PAGE_SIZE, RT_COMPUTE_GTT,
				&s->staging);
	if (!r)
		r = rt_compute_bo_info(ctx, s->staging, &info);
	if (r || !info.cpu_address || !info.gpu_address) {
		if (s->staging && rt_compute_bo_free(ctx, s->staging)) {
			/* An unreleasable staging BO stays owned by the context. */
		}
		session_free(s);
		return r ? r : -ENOMEM;
	}
	s->staging_cpu = info.cpu_address;
	s->staging_mc = info.gpu_address;
	s->proc = linuxu_process_create(pid, comm ? comm : "hsa-client");
	if (!s->proc) {
		(void)rt_compute_bo_free(ctx, s->staging);
		session_free(s);
		return -ENOMEM;
	}
	r = session_enter(s, &saved);
	if (!r) {
		r = session_handshake(s);
		linuxu_process_leave(&saved);
	}
	if (r) {
		/* Exit like a process whose open failed: the KFD process (if any)
		 * is torn down by the notifier release and descriptor close. */
		linuxu_process_exit(s->proc);
		mmu_notifier_synchronize();
		(void)rt_compute_bo_free(ctx, s->staging);
		session_free(s);
		return r;
	}
	*out = s;
	return 0;
}

int rt_kfd_session_uncertain(const struct rt_kfd_session *s)
{
	return s && s->uncertain;
}

int rt_kfd_session_failure(const struct rt_kfd_session *s, int *error)
{
	if (error)
		*error = s ? s->fail_error : 0;
	return s ? (int)s->fail_step : RT_KFD_STEP_NONE;
}

int rt_kfd_session_pid(const struct rt_kfd_session *s)
{
	return s && s->proc ? linuxu_process_leader(s->proc)->pid : 0;
}

int rt_kfd_session_apertures(struct rt_kfd_session *s, struct rt_kfd_apertures *out)
{
	if (!s || !out)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	*out = s->ap;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

int rt_kfd_session_queue_limits(struct rt_kfd_session *s, struct rt_kfd_queue_limits *out)
{
	if (!s || !out)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	*out = s->limits;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

uint64_t rt_kfd_session_window_size(struct rt_kfd_session *s)
{
	return s ? s->window_size : 0;
}

int rt_kfd_session_set_window(struct rt_kfd_session *s, uint64_t base, uint64_t size)
{
	int r = 0;

	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (!size)
		size = s->window_size;
	if (s->uncertain)
		r = -EBUSY;
	else if (s->window_set)
		r = s->window.base == base && s->window.end - s->window.base == size ? 0 : -EBUSY;
	else if (!base || size > s->window_size || (size & (size - 1)) || (base & (size - 1)) ||
		 base < s->ap.gpuvm_base || base > s->private_range.base ||
		 size > s->private_range.base - base)
		r = -EINVAL;
	else {
		s->window.base = base;
		s->window.end = base + size;
		s->window_set = true;
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_session_window(struct rt_kfd_session *s, uint64_t *base, uint64_t *size)
{
	if (!s || !base || !size)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	*base = s->window_set ? s->window.base : 0;
	*size = s->window_set ? s->window.end - s->window.base : s->window_size;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

unsigned int rt_kfd_session_queue_count(struct rt_kfd_session *s)
{
	unsigned int n;

	if (!s)
		return 0;
	pthread_mutex_lock(&s->lock);
	n = s->queue_count;
	pthread_mutex_unlock(&s->lock);
	return n;
}

/* ---- confirming that the GPU let go ---- */

static int session_pid(const struct rt_kfd_session *s)
{
	return s->proc ? linuxu_process_leader(s->proc)->pid : 0;
}

/* The first step that could not confirm the GPU let go names the failure. */
static void note_failure(struct rt_kfd_session *s, enum rt_kfd_step step, int error)
{
	if (s->fail_step == RT_KFD_STEP_NONE) {
		s->fail_step = step;
		s->fail_error = error;
	}
}

/* Uncertain while a copy may still run or a queue may still be scheduled.
 * Caller holds s->lock. */
static void reassess_locked(struct rt_kfd_session *s)
{
	bool failed = false;

	for (struct rt_kfd_queue *q = s->queues; q; q = q->next)
		failed |= q->failed;
	s->uncertain = s->pending_copy || failed;
}

/* A copy that outlived its timeout holds the staging and its BO until its
 * fence signals: wait for it up to @wait_ms more. A late completion, even
 * with an error, means the engine is done with that memory. Caller holds
 * s->lock. */
static void settle_copy_locked(struct rt_kfd_session *s, unsigned int wait_ms)
{
	struct dma_fence *f = s->pending_copy;

	if (!f)
		return;
	if (rt_removal_active(s->adev)) {
		/* A removed device writes no memory: the copy is over. */
		pr_warn("kfd session %d: device removed; an SDMA copy it never finished "
			"no longer holds anything\n", session_pid(s));
		dma_fence_put(f);
		s->pending_copy = NULL;
		return;
	}
	if (!dma_fence_is_signaled(f) &&
	    dma_fence_wait_timeout(f, false, msecs_to_jiffies(wait_ms)) <= 0 &&
	    !dma_fence_is_signaled(f))
		return;
	pr_warn("kfd session %d: an SDMA copy that timed out has completed (status %d); "
		"releasing what it held\n", session_pid(s), dma_fence_get_status(f));
	dma_fence_put(f);
	s->pending_copy = NULL;
}

/* Whether KFD still lists the queue in its device queue manager. A failed
 * DESTROY_QUEUE either changed nothing (the queue is still scheduled and
 * DESTROY_QUEUE may run again) or failed in MES's removal, after
 * destroy_queue_cpsch took the queue off its lists and freed its MQD.
 * Caller holds s->lock. */
static bool queue_listed(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	struct kfd_process_device *pdd;
	struct queue *kq, *it;
	bool listed = false;

	mutex_lock(&s->process->mutex);
	kq = pqm_get_user_queue(&s->process->pqm, q->queue_id);
	pdd = kfd_process_device_data_by_id(s->process, s->ap.gpu_id);
	if (kq && pdd && pdd->dev && pdd->dev->dqm) {
		dqm_lock(pdd->dev->dqm);
		list_for_each_entry(it, &pdd->qpd.queues_list, list) {
			if (it == kq) {
				listed = true;
				break;
			}
		}
		dqm_unlock(pdd->dev->dqm);
	}
	mutex_unlock(&s->process->mutex);
	return listed;
}

/* Upstream's recovery of a hung MES user queue (amdgpu_userq: the
 * detect_and_reset before the unmap). MES resets the compute queues it
 * finds hung, then removes this one again, which it now does even after a
 * reset (remove_queue_after_reset). MES confirming the removal means it no
 * longer schedules the queue. The device queue manager's lock is held as
 * for remove_queue_mes; every MES call is bounded by MES's API timeout.
 * Caller holds s->lock. */
static int queue_recover(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	struct amdgpu_device *adev = s->adev;
	struct kfd_node *node = session_node(adev);
	struct device_queue_manager *dqm = node ? node->dqm : NULL;
	struct mes_remove_queue_input remove;
	uint32_t hung[8];
	unsigned int nhung = 0, mine = 0;
	int reset = -EOPNOTSUPP, r;

	if (!q->mes_known || !dqm || !adev->enable_mes || !adev->mes.funcs ||
	    !adev->mes.funcs->remove_hw_queue)
		return -ENODEV;
	memset(hung, 0xff, sizeof(hung));
	memset(&remove, 0, sizeof(remove));
	dqm_lock(dqm);
	/* As remove_queue_mes: no MES queue operation while the queue manager
	 * is stopped (KFD suspended, rt/power.h) or halted. The stop's own
	 * removals cover only the queues KFD still lists, so this one stays
	 * failed, with its memory, until the manager schedules again. */
	if (!dqm->sched_running || dqm->sched_halt) {
		dqm_unlock(dqm);
		return -EAGAIN;
	}
	if (!down_read_trylock(&adev->reset_domain->sem)) {
		dqm_unlock(dqm);
		return -EIO;
	}
	if (adev->mes.funcs->detect_and_reset_hung_queues &&
	    adev->mes.hung_queue_db_array_size > 0 &&
	    adev->mes.hung_queue_db_array_size <= (int)ARRAY_SIZE(hung)) {
		amdgpu_mes_lock(&adev->mes);
		reset = amdgpu_mes_detect_and_reset_hung_queues(adev, AMDGPU_RING_TYPE_COMPUTE,
								false, &nhung, hung, 0);
		amdgpu_mes_unlock(&adev->mes);
		for (unsigned int i = 0; !reset && i < ARRAY_SIZE(hung); ++i)
			mine += hung[i] == q->mes_doorbell;
		if (reset)
			note_failure(s, RT_KFD_STEP_MES_RESET, reset);
	}
	remove.xcc_id = ffs(node->xcc_mask) - 1;
	remove.doorbell_offset = q->mes_doorbell;
	remove.gang_context_addr = q->mes_gang_ctx;
	remove.remove_queue_after_reset = true;
	amdgpu_mes_lock(&adev->mes);
	r = adev->mes.funcs->remove_hw_queue(&adev->mes, &remove);
	amdgpu_mes_unlock(&adev->mes);
	/* As upstream: scheduling resumes once a hung queue was reset. */
	if (!reset && nhung)
		(void)amdgpu_mes_resume(adev);
	up_read(&adev->reset_domain->sem);
	dqm_unlock(dqm);
	pr_warn("kfd session %d: queue %u (MES doorbell %#x): hung-queue reset %d "
		"(%u hung, %s), removal after reset %d\n", session_pid(s), q->queue_id,
		q->mes_doorbell, reset, nhung, mine ? "this one among them" : "not this one", r);
	return r;
}

/* ---- memory ---- */

static bool bo_owned(struct rt_kfd_session *s, struct rt_kfd_bo *bo)
{
	for (struct rt_kfd_bo *cur = s->bos; cur; cur = cur->next)
		if (cur == bo)
			return true;
	return false;
}

static void *translate_handle(struct rt_kfd_session *s, uint64_t handle)
{
	struct kfd_process_device *pdd;
	void *mem = NULL;

	mutex_lock(&s->process->mutex);
	pdd = kfd_process_device_data_by_id(s->process, GET_GPU_ID(handle));
	if (pdd)
		mem = kfd_process_device_translate_handle(pdd, GET_IDR_HANDLE(handle));
	mutex_unlock(&s->process->mutex);
	return mem;
}

static int unmap_and_free(struct rt_kfd_session *s, uint64_t handle)
{
	struct {
		struct kfd_ioctl_unmap_memory_from_gpu_args args;
		uint32_t devices[1];
	} unmap;
	struct kfd_ioctl_free_memory_of_gpu_args release = {0};
	long r;

	memset(&unmap, 0, sizeof(unmap));
	unmap.args.handle = handle;
	unmap.args.device_ids_array_ptr = arena_address(&unmap, unmap.devices);
	unmap.args.n_devices = 1;
	unmap.devices[0] = s->ap.gpu_id;
	r = session_ioctl(s, AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &unmap, sizeof(unmap));
	release.handle = handle;
	if (!r)
		r = session_ioctl(s, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &release, sizeof(release));
	return (int)r;
}

/* KFD reports why an allocation was refused only at debug level. Say what
 * was asked and what KFD's own limits stood at: its VRAM accounting (used,
 * pinned, available) and the system RAM its system limit derives from. */
static void report_alloc_failure(struct rt_kfd_session *s, uint64_t size,
				 enum rt_kfd_domain domain, long r)
{
	struct amdgpu_device *adev = s->adev;
	struct sysinfo info;

	memset(&info, 0, sizeof(info));
	si_meminfo(&info);
	pr_warn("kfd session: %s %llu bytes refused (%ld%s); KFD VRAM used %lld "
		"pinned %lld available %zu of %llu; VRAM manager used %llu of %llu; RAM %llu\n",
		domain == RT_KFD_VRAM ? "VRAM" : "GTT", (unsigned long long)size, r,
		r == -ENOSPC ? ": would evict, VRAM manager full" : "",
		(long long)adev->kfd.vram_used[0], (long long)atomic64_read(&adev->vram_pin_size),
		amdgpu_amdkfd_get_available_memory(adev, 0),
		(unsigned long long)adev->gmc.real_vram_size,
		(unsigned long long)adev->mman.vram_mgr.manager.usage,
		(unsigned long long)adev->mman.vram_mgr.manager.size,
		(unsigned long long)info.totalram * (info.mem_unit ? info.mem_unit : 1));
}

/* VRAM a KFD allocation must leave free in the VRAM manager: the page
 * tables mapping it, and the kernel's own buffers, also come from VRAM. */
#define RT_KFD_VRAM_HEADROOM (128ULL << 20)

/* Whether the VRAM manager can place @size bytes without evicting anything.
 * KFD admits a VRAM allocation against its own accounting, which counts only
 * KFD buffers; when the manager is full anyway (kernel buffers, page tables,
 * firmware), TTM makes room by evicting this process's buffers to GTT, and on
 * a device behind a narrow, BAR-limited link that move is what hangs the
 * copy engine and loses the device. Refusing here keeps an allocation that
 * does not fit an allocation failure. A manager that reports no size (not
 * initialized) is not checked. */
static bool vram_fits(struct amdgpu_device *adev, uint64_t size)
{
	struct ttm_resource_manager *man = &adev->mman.vram_mgr.manager;
	uint64_t used;

	if (!man->size)
		return true;
	spin_lock(&adev->mman.bdev.lru_lock);
	used = man->usage;
	spin_unlock(&adev->mman.bdev.lru_lock);
	return used <= man->size && size <= man->size - used &&
	       man->size - used - size >= RT_KFD_VRAM_HEADROOM;
}

/* Caller holds s->lock and is inside the process. */
static int bo_alloc_locked(struct rt_kfd_session *s, uint64_t size, uint64_t alignment,
			   enum rt_kfd_domain domain, enum rt_kfd_place place,
			   struct rt_kfd_bo **out)
{
	struct va_region *region = place == RT_KFD_PLACE_WINDOW ? &s->window :
				   &s->private_range;
	struct kfd_ioctl_alloc_memory_of_gpu_args alloc = {0};
	struct {
		struct kfd_ioctl_map_memory_to_gpu_args args;
		uint32_t devices[1];
	} map;
	struct rt_kfd_bo *bo;
	uint64_t va = 0;
	long r;

	*out = NULL;
	if (s->uncertain)
		return -EBUSY;
	if (!size || size > UINT64_MAX - PAGE_SIZE ||
	    (domain != RT_KFD_GTT && domain != RT_KFD_VRAM) ||
	    (place == RT_KFD_PLACE_WINDOW && (!s->window_set || domain != RT_KFD_GTT)))
		return -EINVAL;
	if (alignment & (alignment - 1))
		return -EINVAL;
	size = PAGE_ALIGN(size);
	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;
	if (size >= RT_KFD_LARGE_PAGE && alignment < RT_KFD_LARGE_PAGE)
		alignment = RT_KFD_LARGE_PAGE;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	r = va_alloc(region, size, alignment, &va);
	if (r) {
		kfree(bo);
		return (int)r;
	}
	/* libhsakmt's flags: device-local VRAM, or coherent non-paged system
	 * memory (fine-grained, shared with the host). Both executable, as
	 * code objects and signal kernels run from them. */
	if (domain == RT_KFD_VRAM && !vram_fits(s->adev, size)) {
		report_alloc_failure(s, size, domain, -ENOSPC);
		va_free(region, va);
		kfree(bo);
		return -ENOMEM;
	}
	alloc.va_addr = va;
	alloc.size = size;
	alloc.gpu_id = s->ap.gpu_id;
	alloc.flags = KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
		      KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE |
		      KFD_IOC_ALLOC_MEM_FLAGS_NO_SUBSTITUTE;
	if (domain == RT_KFD_VRAM)
		alloc.flags |= KFD_IOC_ALLOC_MEM_FLAGS_VRAM;
	else
		alloc.flags |= KFD_IOC_ALLOC_MEM_FLAGS_GTT |
			       KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
	r = session_ioctl(s, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &alloc, sizeof(alloc));
	if (r) {
		report_alloc_failure(s, size, domain, r);
		va_free(region, va);
		kfree(bo);
		return (int)r;
	}
	memset(&map, 0, sizeof(map));
	map.args.handle = alloc.handle;
	map.args.device_ids_array_ptr = arena_address(&map, map.devices);
	map.args.n_devices = 1;
	map.devices[0] = s->ap.gpu_id;
	r = session_ioctl(s, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &map, sizeof(map));
	if (!r && map.args.n_success != 1)
		r = -EIO;
	if (!r) {
		bo->mem = translate_handle(s, alloc.handle);
		bo->abo = bo->mem ? bo->mem->bo : NULL;
		if (!bo->abo)
			r = -EFAULT;
	}
	if (r) {
		struct kfd_ioctl_free_memory_of_gpu_args release = {0};

		if (map.args.n_success)
			(void)unmap_and_free(s, alloc.handle);
		else {
			release.handle = alloc.handle;
			(void)session_ioctl(s, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &release,
					    sizeof(release));
		}
		va_free(region, va);
		kfree(bo);
		return (int)r;
	}
	bo->handle = alloc.handle;
	bo->va = va;
	bo->size = size;
	bo->domain = domain;
	bo->place = place;
	bo->next = s->bos;
	s->bos = bo;
	*out = bo;
	return 0;
}

static int bo_free_locked(struct rt_kfd_session *s, struct rt_kfd_bo *bo)
{
	struct rt_kfd_bo **link;
	int r;

	if (bo->queue_uses)
		return -EBUSY;
	for (link = &s->bos; *link && *link != bo; link = &(*link)->next)
		;
	if (!*link)
		return -ENOENT;
	r = unmap_and_free(s, bo->handle);
	if (r)
		return r;
	*link = bo->next;
	va_free(bo->place == RT_KFD_PLACE_WINDOW ? &s->window : &s->private_range, bo->va);
	kfree(bo);
	return 0;
}

int rt_kfd_bo_alloc(struct rt_kfd_session *s, uint64_t size, uint64_t alignment,
		    enum rt_kfd_domain domain, enum rt_kfd_place place,
		    struct rt_kfd_bo **out)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s || !out)
		return -EINVAL;
	*out = NULL;
	pthread_mutex_lock(&s->lock);
	r = session_enter(s, &saved);
	if (!r) {
		r = bo_alloc_locked(s, size, alignment, domain, place, out);
		linuxu_process_leave(&saved);
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_bo_free(struct rt_kfd_session *s, struct rt_kfd_bo *bo)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s || !bo)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (s->uncertain)
		r = -EBUSY;
	else if (!bo_owned(s, bo))
		r = -ENOENT;
	else if (!(r = session_enter(s, &saved))) {
		r = bo_free_locked(s, bo);
		linuxu_process_leave(&saved);
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_bo_info(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		   struct rt_kfd_bo_info *out)
{
	int r = 0;

	if (!s || !bo || !out)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (!bo_owned(s, bo))
		r = -ENOENT;
	else
		*out = (struct rt_kfd_bo_info){ bo->va, bo->size, bo->handle, bo->domain };
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* ---- kernel-side access ----
 * A BO's placement decides the path: system pages (TT, or SYSTEM while still
 * populated) are read and written by the CPU through the pages; VRAM ranges
 * are reached with SDMA copies on the kernel buffer ring to and from the
 * GART-mapped staging. The BO stays reserved, so TTM cannot move it, while a
 * copy runs. */

enum access_kind { ACCESS_CPU, ACCESS_VRAM };

static int bo_access_kind(struct amdgpu_bo *abo, enum access_kind *kind)
{
	struct ttm_resource *res = abo->tbo.resource;
	struct ttm_tt *ttm = abo->tbo.ttm;

	if (!res)
		return -EBUSY;
	if (res->mem_type == TTM_PL_VRAM) {
		*kind = ACCESS_VRAM;
		return 0;
	}
	if ((res->mem_type == TTM_PL_TT || res->mem_type == TTM_PL_SYSTEM) &&
	    ttm && ttm->pages && ttm_tt_is_populated(ttm)) {
		*kind = ACCESS_CPU;
		return 0;
	}
	return -EBUSY;
}

/* CPU pointer for @offset and the bytes contiguous from it within its page. */
static void *bo_cpu(struct amdgpu_bo *abo, uint64_t offset, uint64_t *avail)
{
	struct ttm_tt *ttm = abo->tbo.ttm;
	uint64_t index = offset >> PAGE_SHIFT;
	char *page;

	if (index >= ttm->num_pages || !ttm->pages[index])
		return NULL;
	page = page_address(ttm->pages[index]);
	if (!page)
		return NULL;
	*avail = PAGE_SIZE - (offset & (PAGE_SIZE - 1));
	return page + (offset & (PAGE_SIZE - 1));
}

static int cpu_transfer(struct amdgpu_bo *abo, uint64_t offset, void *buf,
			uint64_t bytes, bool write)
{
	while (bytes) {
		uint64_t avail = 0, n;
		char *p = bo_cpu(abo, offset, &avail);

		if (!p)
			return -EFAULT;
		n = bytes < avail ? bytes : avail;
		if (write)
			memcpy(p, buf, n);
		else
			memcpy(buf, p, n);
		offset += n;
		buf = (char *)buf + n;
		bytes -= n;
	}
	return 0;
}

static int sdma_copy(struct rt_kfd_session *s, uint64_t src, uint64_t dst, uint64_t bytes)
{
	struct amdgpu_device *adev = s->adev;
	struct dma_fence *fence = NULL;
	int r;

	if (!adev->mman.buffer_funcs_enabled || !adev->mman.buffer_funcs_ring ||
	    !adev->mman.buffer_funcs_ring->sched.ready)
		return -ENODEV;
	mutex_lock(&adev->mman.default_entity.lock);
	r = amdgpu_copy_buffer(adev, &adev->mman.default_entity, src, dst,
			       (uint32_t)bytes, NULL, &fence, false, 0);
	mutex_unlock(&adev->mman.default_entity.lock);
	if (!r && !fence)
		r = -EIO;
	if (!r) {
		long waited = dma_fence_wait_timeout(fence, false,
				msecs_to_jiffies(RT_KFD_COPY_TIMEOUT_MS));

		r = waited > 0 ? dma_fence_get_status(fence) :
		    (waited == 0 ? -ETIMEDOUT : (int)waited);
		r = r > 0 ? 0 : (r == 0 ? -EIO : r);
	}
	if (r && fence) {
		/* The copy may still run: staging and the BO stay retained until
		 * its fence signals (settle_copy_locked). Copies on the entity
		 * complete in order, so the latest one covers every earlier one. */
		pr_err("kfd session %d: SDMA copy of %llu bytes did not complete (%d); "
		       "keeping the staging and the buffer\n", session_pid(s),
		       (unsigned long long)bytes, r);
		note_failure(s, RT_KFD_STEP_COPY, r);
		dma_fence_put(s->pending_copy);
		s->pending_copy = dma_fence_get(fence);
		s->uncertain = true;
	}
	dma_fence_put(fence);
	return r;
}

/* SDMA between a VRAM BO range and a contiguous MC range, segment by
 * segment of the BO's VRAM allocation. */
static int vram_transfer(struct rt_kfd_session *s, struct amdgpu_bo *abo,
			 uint64_t offset, uint64_t mc, uint64_t bytes, bool to_bo)
{
	const uint64_t vram = amdgpu_ttm_domain_start(s->adev, TTM_PL_VRAM);
	struct amdgpu_res_cursor cur;

	amdgpu_res_first(abo->tbo.resource, offset, bytes, &cur);
	while (cur.remaining) {
		uint64_t n = cur.size;
		int r = to_bo ? sdma_copy(s, mc, vram + cur.start, n) :
				sdma_copy(s, vram + cur.start, mc, n);

		if (r)
			return r;
		mc += n;
		amdgpu_res_next(&cur, n);
	}
	return 0;
}

static int vram_to_vram(struct rt_kfd_session *s, struct amdgpu_bo *src, uint64_t so,
			struct amdgpu_bo *dst, uint64_t dof, uint64_t bytes)
{
	const uint64_t vram = amdgpu_ttm_domain_start(s->adev, TTM_PL_VRAM);
	struct amdgpu_res_cursor a, b;

	amdgpu_res_first(src->tbo.resource, so, bytes, &a);
	amdgpu_res_first(dst->tbo.resource, dof, bytes, &b);
	while (a.remaining) {
		uint64_t n = a.size < b.size ? a.size : b.size;
		int r = sdma_copy(s, vram + a.start, vram + b.start, n);

		if (r)
			return r;
		amdgpu_res_next(&a, n);
		amdgpu_res_next(&b, n);
	}
	return 0;
}

static int reserve_pair(struct amdgpu_bo *a, struct amdgpu_bo *b)
{
	int r = amdgpu_bo_reserve(a, false);

	if (r || a == b)
		return r;
	r = amdgpu_bo_reserve(b, false);
	if (r)
		amdgpu_bo_unreserve(a);
	return r;
}

static void unreserve_pair(struct amdgpu_bo *a, struct amdgpu_bo *b)
{
	if (a != b)
		amdgpu_bo_unreserve(b);
	amdgpu_bo_unreserve(a);
}

/* Host buffer <-> BO; caller holds s->lock. */
static int bo_host_transfer(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
			    uint64_t offset, void *buf, uint64_t bytes, bool write)
{
	enum access_kind kind;
	int r;

	if (s->uncertain)
		return -EBUSY;
	if (!bo_owned(s, bo))
		return -ENOENT;
	if (offset > bo->size || bytes > bo->size - offset)
		return -ERANGE;
	if (!bytes)
		return 0;
	r = amdgpu_bo_reserve(bo->abo, false);
	if (r)
		return r;
	r = bo_access_kind(bo->abo, &kind);
	if (!r && kind == ACCESS_CPU)
		r = cpu_transfer(bo->abo, offset, buf, bytes, write);
	else if (!r) {
		while (bytes && !r) {
			uint64_t n = bytes < RT_KFD_STAGING_BYTES ? bytes : RT_KFD_STAGING_BYTES;

			if (write) {
				memcpy(s->staging_cpu, buf, n);
				r = vram_transfer(s, bo->abo, offset, s->staging_mc, n, true);
			} else {
				r = vram_transfer(s, bo->abo, offset, s->staging_mc, n, false);
				if (!r)
					memcpy(buf, s->staging_cpu, n);
			}
			offset += n;
			buf = (char *)buf + n;
			bytes -= n;
		}
	}
	amdgpu_bo_unreserve(bo->abo);
	return r;
}

int rt_kfd_bo_read(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		   uint64_t offset, void *dst, size_t bytes)
{
	int r;

	if (!s || !bo || (!dst && bytes))
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	r = bo_host_transfer(s, bo, offset, dst, bytes, false);
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_bo_write(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
		    uint64_t offset, const void *src, size_t bytes)
{
	int r;

	if (!s || !bo || (!src && bytes))
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	r = bo_host_transfer(s, bo, offset, (void *)src, bytes, true);
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_bo_copy(struct rt_kfd_session *s, struct rt_kfd_bo *src,
		   uint64_t src_offset, struct rt_kfd_bo *dst,
		   uint64_t dst_offset, uint64_t bytes)
{
	enum access_kind sk, dk;
	int r;

	if (!s || !src || !dst)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (s->uncertain)
		r = -EBUSY;
	else if (!bo_owned(s, src) || !bo_owned(s, dst))
		r = -ENOENT;
	else if (!bytes || src_offset > src->size || bytes > src->size - src_offset ||
		 dst_offset > dst->size || bytes > dst->size - dst_offset ||
		 (src == dst && src_offset < dst_offset + bytes &&
		  dst_offset < src_offset + bytes))
		r = -EINVAL;
	else
		r = reserve_pair(src->abo, dst->abo);
	if (r) {
		pthread_mutex_unlock(&s->lock);
		return r;
	}
	r = bo_access_kind(src->abo, &sk);
	if (!r)
		r = bo_access_kind(dst->abo, &dk);
	if (!r && sk == ACCESS_VRAM && dk == ACCESS_VRAM)
		r = vram_to_vram(s, src->abo, src_offset, dst->abo, dst_offset, bytes);
	while (!r && bytes && !(sk == ACCESS_VRAM && dk == ACCESS_VRAM)) {
		uint64_t n = bytes < RT_KFD_STAGING_BYTES ? bytes : RT_KFD_STAGING_BYTES;

		if (sk == ACCESS_CPU && dk == ACCESS_CPU) {
			/* Page by page, through the staging for overlap-free
			 * semantics across the two page lists. */
			r = cpu_transfer(src->abo, src_offset, s->staging_cpu, n, false);
			if (!r)
				r = cpu_transfer(dst->abo, dst_offset, s->staging_cpu, n, true);
		} else if (sk == ACCESS_CPU) {
			r = cpu_transfer(src->abo, src_offset, s->staging_cpu, n, false);
			if (!r)
				r = vram_transfer(s, dst->abo, dst_offset, s->staging_mc, n, true);
		} else {
			r = vram_transfer(s, src->abo, src_offset, s->staging_mc, n, false);
			if (!r)
				r = cpu_transfer(dst->abo, dst_offset, s->staging_cpu, n, true);
		}
		src_offset += n;
		dst_offset += n;
		bytes -= n;
	}
	unreserve_pair(src->abo, dst->abo);
	pthread_mutex_unlock(&s->lock);
	return r;
}

int rt_kfd_bo_cpu_ranges(struct rt_kfd_session *s, struct rt_kfd_bo *bo,
			 int (*fn)(void *arg, void *cpu, uint64_t bytes), void *arg)
{
	enum access_kind kind;
	int r;

	if (!s || !bo || !fn)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (!bo_owned(s, bo) || bo->domain != RT_KFD_GTT) {
		pthread_mutex_unlock(&s->lock);
		return -EINVAL;
	}
	r = amdgpu_bo_reserve(bo->abo, false);
	if (!r) {
		r = bo_access_kind(bo->abo, &kind);
		if (!r && kind != ACCESS_CPU)
			r = -EINVAL;
		for (uint64_t offset = 0; !r && offset < bo->size;) {
			uint64_t avail = 0, run;
			char *start = bo_cpu(bo->abo, offset, &avail);

			if (!start) {
				r = -EFAULT;
				break;
			}
			run = avail;
			while (offset + run < bo->size) {
				uint64_t next_avail = 0;
				char *next = bo_cpu(bo->abo, offset + run, &next_avail);

				if (next != start + run)
					break;
				run += next_avail;
			}
			if (run > bo->size - offset)
				run = bo->size - offset;
			r = fn(arg, start, run);
			offset += run;
		}
		amdgpu_bo_unreserve(bo->abo);
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* ---- GPU memory faults ---- */

/* A zero-timeout WAIT_EVENTS on the memory event: KFD copies the exception
 * data of a signaled memory event into the wait's event record. Caller
 * holds s->lock and is inside the process. */
static int fault_poll_locked(struct rt_kfd_session *s)
{
	struct {
		struct kfd_ioctl_wait_events_args args;
		struct kfd_event_data event;
	} wait;
	const struct kfd_hsa_memory_exception_data *data;
	long r;

	if (s->faulted)
		return 1;
	/* Not signaled: no fault, and no call. Once KFD signaled it, the
	 * zero-timeout wait reads what it reported. */
	if (!READ_ONCE(s->memory_ev->signaled))
		return 0;
	memset(&wait, 0, sizeof(wait));
	wait.args.events_ptr = arena_address(&wait, &wait.event);
	wait.args.num_events = 1;
	wait.args.wait_for_all = 1;
	wait.args.timeout = 0;
	wait.event.event_id = s->memory_event;
	r = session_ioctl(s, AMDKFD_IOC_WAIT_EVENTS, &wait, sizeof(wait));
	if (r)
		return (int)r;
	if (wait.args.wait_result != KFD_IOC_WAIT_RESULT_COMPLETE)
		return 0;
	data = &wait.event.memory_exception_data;
	s->faulted = true;
	s->fault = (struct rt_kfd_fault){
		.va = data->va,
		.not_present = data->failure.NotPresent,
		.read_only = data->failure.ReadOnly,
		.no_execute = data->failure.NoExecute,
		.imprecise = data->failure.imprecise,
	};
	pr_err("kfd session %d: GPU memory fault at %#llx (%s%s%s); KFD evicted the process's "
	       "queues, which do not run again\n", session_pid(s),
	       (unsigned long long)s->fault.va,
	       s->fault.not_present ? "page not present" : "protection",
	       s->fault.read_only ? ", write to read-only" : "",
	       s->fault.no_execute ? ", no execute" : "");
	return 1;
}

int rt_kfd_session_fault(struct rt_kfd_session *s, struct rt_kfd_fault *out)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (s->faulted) {
		r = 1;
	} else if (s->closing) {
		r = -ESHUTDOWN;
	} else {
		r = session_enter(s, &saved);
		if (!r) {
			r = fault_poll_locked(s);
			linuxu_process_leave(&saved);
		}
	}
	if (r == 1 && out)
		*out = s->fault;
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* ---- queues ---- */

/* libhsakmt maps the process doorbell slice once per GPU before the first
 * queue's doorbell is used. kfd_doorbell_mmap() io_remap_pfn_range()s the
 * slice's bus address into the VMA; that recorded range locates the
 * doorbells on the BAR the kernel mapping covers. Caller is inside the
 * process. */
static int map_doorbells(struct rt_kfd_session *s)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct file *file;
	uint64_t bus;
	int r;

	if (s->doorbell_mapped)
		return 0;
	file = session_kfd_file(s);
	if (!file)
		return -EBADF;
	vma = vm_area_alloc(mm);
	if (!vma) {
		fput(file);
		return -ENOMEM;
	}
	vma->vm_start = RT_KFD_DOORBELL_VA;
	vma->vm_end = RT_KFD_DOORBELL_VA + s->limits.doorbell_slice_bytes;
	vma->vm_pgoff = (KFD_MMAP_TYPE_DOORBELL | KFD_MMAP_GPU_ID(s->ap.gpu_id)) >> PAGE_SHIFT;
	vma->vm_flags = VM_READ | VM_WRITE | VM_SHARED;
	vma->vm_file = file;	/* the mapping's file reference */
	r = file->f_op && file->f_op->mmap ? file->f_op->mmap(file, vma) : -ENODEV;
	if (!r && (!vma->linuxu_pfn_bytes ||
		   vma->linuxu_pfn_bytes != vma->vm_end - vma->vm_start))
		r = -EIO;
	if (!r) {
		bus = (uint64_t)vma->linuxu_pfn << PAGE_SHIFT;
		if (bus < s->adev->doorbell.base ||
		    bus - s->adev->doorbell.base >= s->adev->doorbell.size ||
		    s->limits.doorbell_slice_bytes > s->adev->doorbell.size -
						     (bus - s->adev->doorbell.base))
			r = -ERANGE;
		else
			s->doorbell_first = (uint32_t)((bus - s->adev->doorbell.base) / sizeof(u32));
	}
	if (!r) {
		mmap_write_lock(mm);
		r = linuxu_mm_insert_vma(mm, vma);
		mmap_write_unlock(mm);
	}
	if (r) {
		fput(file);
		vm_area_free(vma);
		return r;
	}
	s->doorbell_mapped = true;
	return 0;
}

static bool va_in_bo(const struct rt_kfd_bo *bo, uint64_t va, uint64_t bytes)
{
	return va >= bo->va && va - bo->va <= bo->size && bytes <= bo->size - (va - bo->va);
}

/* libhsakmt's fill_cwsr_header for each XCC's save area. */
static int fill_ctx_header(struct rt_kfd_session *s, struct rt_kfd_bo *ctx)
{
	const struct rt_kfd_queue_limits *l = &s->limits;

	for (uint32_t i = 0; i < l->xcc_count; ++i) {
		struct {
			uint32_t control_stack_offset, control_stack_size;
			uint32_t wave_state_offset, wave_state_size;
			uint32_t debug_offset, debug_size;
			uint64_t error_reason;
			uint32_t error_event_id, reserved1;
		} header = {0};
		int r;

		header.debug_offset = (l->xcc_count - i) * l->ctx_save_bytes;
		header.debug_size = l->debug_bytes * l->xcc_count;
		r = amdgpu_bo_reserve(ctx->abo, false);
		if (r)
			return r;
		r = cpu_transfer(ctx->abo, (uint64_t)i * l->ctx_save_bytes, &header,
				 sizeof(header), true);
		amdgpu_bo_unreserve(ctx->abo);
		if (r)
			return r;
	}
	return 0;
}

/* What MES knows the new queue by: KFD's doorbell offset and gang context,
 * as remove_queue_mes passes them. Caller holds s->lock. */
static void queue_identity(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	struct queue *kq;

	mutex_lock(&s->process->mutex);
	kq = pqm_get_user_queue(&s->process->pqm, q->queue_id);
	if (kq) {
		q->mes_doorbell = kq->properties.doorbell_off;
		q->mes_gang_ctx = kq->gang_ctx_gpu_addr;
		q->mes_known = true;
	}
	mutex_unlock(&s->process->mutex);
}

static int queue_destroy_locked(struct rt_kfd_session *s, struct rt_kfd_queue *q);
static void queue_free_locked(struct rt_kfd_session *s, struct rt_kfd_queue *q);

int rt_kfd_queue_create(struct rt_kfd_session *s, const struct rt_kfd_queue_desc *desc,
			struct rt_kfd_queue **out)
{
	struct linuxu_process_saved saved;
	struct kfd_ioctl_create_queue_args args = {0};
	struct rt_kfd_queue *q;
	long r;

	if (!s || !desc || !out || !desc->ring)
		return -EINVAL;
	*out = NULL;
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	pthread_mutex_lock(&s->lock);
	if (s->uncertain)
		r = -EBUSY;
	else if (!bo_owned(s, desc->ring) || !desc->ring_bytes ||
		 (desc->ring_bytes & (desc->ring_bytes - 1)) ||
		 desc->ring->size < desc->ring_bytes || desc->priority > KFD_MAX_QUEUE_PRIORITY)
		r = -EINVAL;
	else if (s->queue_count >= s->limits.slots)
		r = -ENOSPC;
	else
		r = session_enter(s, &saved);
	/* A process whose GPU work faulted runs nothing again: KFD keeps a new
	 * queue of it evicted, so creating one is refused. */
	if (!r) {
		const int faulted = fault_poll_locked(s);

		if (faulted) {
			linuxu_process_leave(&saved);
			r = faulted == 1 ? -EFAULT : faulted;
		}
	}
	if (r) {
		pthread_mutex_unlock(&s->lock);
		kfree(q);
		return (int)r;
	}
	q->ring = desc->ring;
	if (s->limits.eop_bytes)
		r = bo_alloc_locked(s, s->limits.eop_bytes, 0, RT_KFD_VRAM,
				    RT_KFD_PLACE_PRIVATE, &q->eop);
	if (!r)
		r = bo_alloc_locked(s, s->limits.ctx_area_bytes, 0, RT_KFD_GTT,
				    RT_KFD_PLACE_PRIVATE, &q->ctx);
	if (!r)
		r = fill_ctx_header(s, q->ctx);
	if (!r) {
		args.gpu_id = s->ap.gpu_id;
		args.queue_type = KFD_IOC_QUEUE_TYPE_COMPUTE_AQL;
		args.ring_base_address = desc->ring->va;
		args.ring_size = desc->ring_bytes;
		args.read_pointer_address = desc->read_pointer;
		args.write_pointer_address = desc->write_pointer;
		args.queue_percentage = KFD_MAX_QUEUE_PERCENTAGE;
		args.queue_priority = desc->priority ? desc->priority : RT_KFD_PRIORITY_NORMAL;
		if (q->eop) {
			args.eop_buffer_address = q->eop->va;
			args.eop_buffer_size = s->limits.eop_bytes;
		}
		args.ctx_save_restore_address = q->ctx->va;
		args.ctx_save_restore_size = s->limits.ctx_save_bytes;
		args.ctl_stack_size = s->limits.ctl_stack_bytes;
		r = session_ioctl(s, AMDKFD_IOC_CREATE_QUEUE, &args, sizeof(args));
	}
	if (r) {
		if (q->ctx)
			(void)bo_free_locked(s, q->ctx);
		if (q->eop)
			(void)bo_free_locked(s, q->eop);
		linuxu_process_leave(&saved);
		pthread_mutex_unlock(&s->lock);
		kfree(q);
		return (int)r;
	}
	q->queue_id = args.queue_id;
	queue_identity(s, q);
	r = map_doorbells(s);
	if (!r) {
		q->doorbell_offset = args.doorbell_offset;
		q->doorbell_index = s->doorbell_first +
			(uint32_t)((args.doorbell_offset & (s->limits.doorbell_slice_bytes - 1)) /
				   sizeof(u32));
	}
	q->ring->queue_uses++;
	if (q->eop)
		q->eop->queue_uses++;
	q->ctx->queue_uses++;
	/* KFD validated that the pointers lie in BOs of the process; those BOs
	 * stay in use while the queue exists. */
	for (struct rt_kfd_bo *bo = s->bos; bo; bo = bo->next) {
		if (bo == q->ring || bo == q->eop || bo == q->ctx ||
		    !(va_in_bo(bo, desc->read_pointer, 8) ||
		      va_in_bo(bo, desc->write_pointer, 8)))
			continue;
		if (!q->pointers[0])
			q->pointers[0] = bo;
		else if (!q->pointers[1])
			q->pointers[1] = bo;
		bo->queue_uses++;
	}
	q->next = s->queues;
	s->queues = q;
	s->queue_count++;
	if (r) {
		/* No doorbell: the queue goes again, as any queue does. One MES
		 * cannot confirm removing stays linked and failed. */
		if (!queue_destroy_locked(s, q))
			queue_free_locked(s, q);
		linuxu_process_leave(&saved);
		pthread_mutex_unlock(&s->lock);
		return (int)r;
	}
	linuxu_process_leave(&saved);
	pthread_mutex_unlock(&s->lock);
	*out = q;
	return 0;
}

int rt_kfd_queue_info(struct rt_kfd_session *s, struct rt_kfd_queue *q,
		      struct rt_kfd_queue_info *out)
{
	int r = -ENOENT;

	if (!s || !q || !out)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	for (struct rt_kfd_queue *cur = s->queues; cur; cur = cur->next) {
		if (cur != q)
			continue;
		*out = (struct rt_kfd_queue_info){
			.queue_id = q->queue_id,
			.doorbell_index = q->doorbell_index,
			.doorbell_offset = q->doorbell_offset,
			.eop_va = q->eop ? q->eop->va : 0,
			.ctx_save_va = q->ctx->va,
		};
		r = 0;
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

static bool queue_owned(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	for (struct rt_kfd_queue *cur = s->queues; cur; cur = cur->next)
		if (cur == q)
			return true;
	return false;
}

int rt_kfd_queue_kick(struct rt_kfd_session *s, struct rt_kfd_queue *q, uint64_t value)
{
	struct amdgpu_device *adev;
	int r = 0;

	if (!s || !q)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	adev = s->adev;
	if (s->uncertain)
		r = -EBUSY;
	else if (!queue_owned(s, q))
		r = -ENOENT;
	else if (q->gone || q->failed)
		r = -ENODEV;
	else if (s->faulted)
		r = -EFAULT;
	else if (!adev->doorbell.cpu_addr ||
		 (uint64_t)q->doorbell_index + 2 > adev->doorbell.size / sizeof(u32))
		r = -ERANGE;
	if (!r) {
		/* Ring, packets and the write index are coherent host memory:
		 * order them before the doorbell write, as ROCr's release store
		 * does. 64-bit doorbells on SOC15 (device_info.doorbell_size). */
		mb();
		writeq(value, adev->doorbell.cpu_addr + q->doorbell_index);
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* Take @q off the GPU: DESTROY_QUEUE, as libhsakmt's hsaKmtDestroyQueue;
 * when MES does not confirm the removal, the hung-queue recovery (once KFD
 * let go of the queue) or, when KFD did not, a later retry of
 * DESTROY_QUEUE. Success marks @q gone; failure marks it failed, and it
 * keeps everything it uses. Caller holds s->lock and is inside the
 * process. */
static int queue_release_locked(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	long r = 0;

	if (q->gone)
		return 0;
	if (!q->detached) {
		struct kfd_ioctl_destroy_queue_args destroy = { .queue_id = q->queue_id };

		r = session_ioctl(s, AMDKFD_IOC_DESTROY_QUEUE, &destroy, sizeof(destroy));
		if (r) {
			note_failure(s, RT_KFD_STEP_DESTROY_QUEUE, (int)r);
			q->detached = !queue_listed(s, q);
			pr_err("kfd session %d: DESTROY_QUEUE %u (MES doorbell %#x) failed (%ld); %s\n",
			       session_pid(s), q->queue_id, q->mes_doorbell, r,
			       q->detached ? "KFD let go of it, recovering it through MES" :
					     "KFD still schedules it");
		}
	}
	if (r && rt_removal_active(s->adev)) {
		/* The device left the bus: nothing can run the queue or reach
		 * the memory it uses any more. */
		pr_warn("kfd session %d: queue %u dropped: the device was removed\n",
			session_pid(s), q->queue_id);
		q->detached = true;
		r = 0;
	} else if (q->detached && rt_removal_active(s->adev)) {
		r = 0;
	} else if (q->detached) {
		r = queue_recover(s, q);
		if (r == -EAGAIN)
			pr_warn("kfd session %d: queue %u: KFD's queue manager is stopped; its "
				"recovery waits for the resume, keeping its memory\n",
				session_pid(s), q->queue_id);
		else if (r) {
			note_failure(s, RT_KFD_STEP_MES_REMOVE, (int)r);
			pr_err("kfd session %d: queue %u: MES did not let go (%ld); keeping the "
			       "queue and the memory it uses\n", session_pid(s), q->queue_id, r);
		}
	}
	if (r) {
		q->failed = true;
		s->uncertain = true;
		return (int)r;
	}
	if (q->failed)
		pr_warn("kfd session %d: queue %u recovered\n", session_pid(s), q->queue_id);
	q->failed = false;
	q->gone = true;
	return 0;
}

/* Take @q off the GPU and unlink it; the caller frees it
 * (queue_free_locked). Caller holds s->lock and is inside the process. */
static int queue_destroy_locked(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	struct rt_kfd_queue **link;
	int r;

	for (link = &s->queues; *link && *link != q; link = &(*link)->next)
		;
	if (!*link)
		return -ENOENT;
	r = queue_release_locked(s, q);
	if (r)
		return r;
	*link = q->next;
	s->queue_count--;
	for (unsigned int i = 0; i < ARRAY_SIZE(q->pointers); ++i)
		if (q->pointers[i])
			q->pointers[i]->queue_uses--;
	q->ring->queue_uses--;
	if (q->eop)
		q->eop->queue_uses--;
	q->ctx->queue_uses--;
	reassess_locked(s);
	return 0;
}

/* The buffers the session allocated for a destroyed queue, then the record.
 * Caller holds s->lock and is inside the process. */
static void queue_free_locked(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	(void)bo_free_locked(s, q->ctx);
	if (q->eop)
		(void)bo_free_locked(s, q->eop);
	kfree(q);
}

int rt_kfd_queue_destroy(struct rt_kfd_session *s, struct rt_kfd_queue *q)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s || !q)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	/* Allowed while uncertain: destroying a queue only lets go of it, and
	 * retries the recovery of one whose removal failed. */
	if (!queue_owned(s, q))
		r = -ENOENT;
	else
		r = session_enter(s, &saved);
	if (r) {
		pthread_mutex_unlock(&s->lock);
		return r;
	}
	r = queue_destroy_locked(s, q);
	if (!r)
		queue_free_locked(s, q);
	linuxu_process_leave(&saved);
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* Retry what made the session uncertain: the copy that outlived its
 * timeout, then every queue whose removal failed. A queue recovered here is
 * no longer scheduled but stays linked, since its owner may still name it.
 * Caller holds s->lock and is inside the process. */
static void settle_locked(struct rt_kfd_session *s, unsigned int wait_ms)
{
	settle_copy_locked(s, wait_ms);
	for (struct rt_kfd_queue *q = s->queues; q; q = q->next)
		if (q->failed)
			(void)queue_release_locked(s, q);
	reassess_locked(s);
}

int rt_kfd_session_settle(struct rt_kfd_session *s, unsigned int wait_ms)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	r = session_enter(s, &saved);
	if (!r) {
		settle_locked(s, wait_ms);
		linuxu_process_leave(&saved);
		r = s->uncertain ? -EBUSY : 0;
	}
	pthread_mutex_unlock(&s->lock);
	return r;
}

/* ---- signal events and interrupt-driven waits ---- */

static long event_destroy_locked(struct rt_kfd_session *s, uint32_t id)
{
	struct kfd_ioctl_destroy_event_args args = { .event_id = id };
	long r = session_ioctl(s, AMDKFD_IOC_DESTROY_EVENT, &args, sizeof(args));

	if (!r) {
		clear_bit(id, s->event_ids);
		s->events--;
	}
	return r;
}

/* Close's first step. Caller holds s->lock and is inside the process. */
static void end_waits_locked(struct rt_kfd_session *s)
{
	s->closing = true;
	for (uint32_t id = 0; id < KFD_SIGNAL_EVENT_LIMIT && s->events; ++id)
		if (test_bit(id, s->event_ids) && event_destroy_locked(s, id)) {
			/* KFD keeps an event it will not destroy; the process
			 * exit frees it. */
			clear_bit(id, s->event_ids);
			s->events--;
		}
	while (s->waits)
		pthread_cond_wait(&s->waits_done, &s->lock);
}

int rt_kfd_event_create(struct rt_kfd_session *s, struct rt_kfd_event *out)
{
	struct kfd_ioctl_create_event_args args = {0};
	struct linuxu_process_saved saved;
	long r;

	if (!s || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&s->lock);
	if (s->uncertain || s->closing) {
		pthread_mutex_unlock(&s->lock);
		return -EBUSY;
	}
	r = session_enter(s, &saved);
	if (r) {
		pthread_mutex_unlock(&s->lock);
		return (int)r;
	}
	if (!s->event_page)
		r = bo_alloc_locked(s, RT_KFD_EVENT_PAGE_BYTES, PAGE_SIZE, RT_KFD_GTT,
				    RT_KFD_PLACE_PRIVATE, &s->event_page);
	if (!r) {
		/* hsaKmtCreateEvent: the event page goes with the first
		 * signal event; KFD maps it and fills it with
		 * UNSIGNALED_EVENT_SLOT. */
		args.event_type = KFD_IOC_EVENT_SIGNAL;
		args.auto_reset = 1;
		args.node_id = 0;
		args.event_page_offset = s->process->signal_page ? 0 : s->event_page->handle;
		r = session_ioctl(s, AMDKFD_IOC_CREATE_EVENT, &args, sizeof(args));
	}
	if (!r && (args.event_id >= KFD_SIGNAL_EVENT_LIMIT ||
		   args.event_slot_index >= KFD_SIGNAL_EVENT_LIMIT)) {
		(void)event_destroy_locked(s, args.event_id);
		r = -ERANGE;
	} else if (!r) {
		set_bit(args.event_id, s->event_ids);
		s->events++;
		out->id = args.event_id;
		out->trigger = args.event_trigger_data;
		out->mailbox_va = s->event_page->va + (uint64_t)args.event_slot_index * 8;
	}
	linuxu_process_leave(&saved);
	pthread_mutex_unlock(&s->lock);
	return (int)r;
}

static int event_call(struct rt_kfd_session *s, uint32_t id, bool destroy)
{
	struct linuxu_process_saved saved;
	long r;

	if (!s || id >= KFD_SIGNAL_EVENT_LIMIT)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (!test_bit(id, s->event_ids)) {
		pthread_mutex_unlock(&s->lock);
		return -ENOENT;
	}
	r = session_enter(s, &saved);
	if (!r) {
		if (destroy) {
			r = event_destroy_locked(s, id);
		} else {
			struct kfd_ioctl_set_event_args args = { .event_id = id };

			r = session_ioctl(s, AMDKFD_IOC_SET_EVENT, &args, sizeof(args));
		}
		linuxu_process_leave(&saved);
	}
	pthread_mutex_unlock(&s->lock);
	return (int)r;
}

int rt_kfd_event_destroy(struct rt_kfd_session *s, uint32_t id)
{
	return event_call(s, id, true);
}

int rt_kfd_event_set(struct rt_kfd_session *s, uint32_t id)
{
	return event_call(s, id, false);
}

unsigned int rt_kfd_event_count(struct rt_kfd_session *s)
{
	unsigned int n;

	if (!s)
		return 0;
	pthread_mutex_lock(&s->lock);
	n = s->events;
	pthread_mutex_unlock(&s->lock);
	return n;
}

int rt_kfd_wait_begin(struct rt_kfd_session *s, const uint32_t *ids, uint32_t count,
		      int all, uint32_t timeout_ms, struct rt_kfd_wait **out)
{
	struct rt_kfd_wait *w;
	int r = 0;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!s || !ids || !count || count > RT_KFD_WAIT_EVENTS_MAX)
		return -EINVAL;
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	pthread_mutex_lock(&s->lock);
	if (s->closing)
		r = -ESHUTDOWN;
	else if (s->uncertain)
		r = -EBUSY;
	else if (s->wait_slots == UINT64_MAX)
		r = -EBUSY;
	for (uint32_t i = 0; !r && i < count; ++i)
		if (ids[i] >= KFD_SIGNAL_EVENT_LIMIT || !test_bit(ids[i], s->event_ids))
			r = -ENOENT;
	if (!r) {
		w->slot = (unsigned int)__builtin_ctzll(~s->wait_slots);
		s->wait_slots |= 1ULL << w->slot;
		s->waits++;
	}
	pthread_mutex_unlock(&s->lock);
	if (r) {
		kfree(w);
		return r;
	}
	w->s = s;
	w->count = count;
	w->all = all ? 1 : 0;
	w->timeout_ms = timeout_ms > RT_KFD_WAIT_MAX_MS ? RT_KFD_WAIT_MAX_MS : timeout_ms;
	memcpy(w->ids, ids, count * sizeof(*ids));
	*out = w;
	return 0;
}

static void wait_end(struct rt_kfd_wait *w)
{
	struct rt_kfd_session *s = w->s;

	pthread_mutex_lock(&s->lock);
	s->wait_slots &= ~(1ULL << w->slot);
	if (!--s->waits)
		pthread_cond_broadcast(&s->waits_done);
	pthread_mutex_unlock(&s->lock);
	kfree(w);
}

void rt_kfd_wait_cancel(struct rt_kfd_wait *w)
{
	if (w)
		wait_end(w);
}

int rt_kfd_wait_run(struct rt_kfd_wait *w, uint32_t *result)
{
	struct {
		struct kfd_ioctl_wait_events_args args;
		struct kfd_event_data events[RT_KFD_WAIT_EVENTS_MAX];
	} *call;
	struct linuxu_process_saved saved;
	struct rt_kfd_session *s;
	size_t bytes;
	long r;

	if (!w)
		return -EINVAL;
	s = w->s;
	if (result)
		*result = KFD_IOC_WAIT_RESULT_FAIL;
	call = kzalloc(sizeof(*call), GFP_KERNEL);
	r = call ? 0 : -ENOMEM;
	if (!r) {
		const uint64_t va = RT_KFD_WAIT_VA + w->slot * RT_KFD_WAIT_STRIDE;

		bytes = offsetof(typeof(*call), events) + w->count * sizeof(call->events[0]);
		call->args.events_ptr = va + offsetof(typeof(*call), events);
		call->args.num_events = w->count;
		call->args.wait_for_all = w->all;
		call->args.timeout = w->timeout_ms;
		for (uint32_t i = 0; i < w->count; ++i)
			call->events[i].event_id = w->ids[i];
		r = session_enter(s, &saved);
		if (!r) {
			/* Unlocked: the KFD process serializes its own event
			 * state (p->event_mutex), and this sleep must not hold
			 * up the session's other calls. */
			r = READ_ONCE(s->closing) ? -ESHUTDOWN :
				rt_process_ioctl(s->kfd_fd, AMDKFD_IOC_WAIT_EVENTS, va, call, bytes);
			linuxu_process_leave(&saved);
		}
		if (!r && result)
			*result = call->args.wait_result;
		kfree(call);
	}
	wait_end(w);
	return (int)r;
}


int rt_kfd_session_close(struct rt_kfd_session *s)
{
	struct linuxu_process_saved saved;
	int r;

	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	r = session_enter(s, &saved);
	if (r) {
		pthread_mutex_unlock(&s->lock);
		return r;
	}
	/* Waits end first: destroying the events wakes each with FAIL, and
	 * none may still be inside the process when it exits. */
	end_waits_locked(s);
	/* What a previous call left uncertain first: a copy that outlived its
	 * timeout gets one more bounded wait. */
	settle_copy_locked(s, RT_KFD_SETTLE_MS);
	/* Every queue off the GPU before any memory goes: no BO may be freed
	 * while MES could still run a queue that references it. As when a
	 * Linux process dies with live queues, each queue is removed, and one
	 * MES does not confirm removing is recovered (queue_release_locked).
	 * Every queue is tried even after one fails. */
	for (struct rt_kfd_queue *q = s->queues, *next; q; q = next) {
		next = q->next;
		if (!queue_destroy_locked(s, q))
			queue_free_locked(s, q);
	}
	reassess_locked(s);
	if (s->uncertain) {
		int error = 0;
		int step = rt_kfd_session_failure(s, &error);

		pr_err("kfd session %d: close kept the session: %s (first failed step %d, "
		       "error %d)\n", session_pid(s),
		       s->pending_copy ? "an SDMA copy is still running" :
					 "MES did not confirm removing a queue", step, error);
		linuxu_process_leave(&saved);
		pthread_mutex_unlock(&s->lock);
		return -EBUSY;
	}
	/* Then the memory. A BO KFD refuses to free stays with the process,
	 * whose release frees every outstanding BO. */
	while (s->bos) {
		struct rt_kfd_bo *bo = s->bos;

		bo->queue_uses = 0;
		if (bo_free_locked(s, bo)) {
			s->bos = bo->next;
			kfree(bo);
		}
	}
	linuxu_process_leave(&saved);
	pthread_mutex_unlock(&s->lock);
	if (s->fail_step != RT_KFD_STEP_NONE)
		pr_warn("kfd session %d: closed cleanly after recovering from step %d "
			"(error %d)\n", session_pid(s), s->fail_step, s->fail_error);
	/* exit_mm then exit_files, as when the process dies: the notifier
	 * release dequeues and tears down the KFD process, closing the KFD and
	 * render descriptors drops the open's and ACQUIRE_VM's references,
	 * and the KFD process is freed by its release work. */
	linuxu_process_exit(s->proc);
	s->proc = NULL;
	mmu_notifier_synchronize();
	if (rt_compute_bo_free(s->ctx, s->staging)) {
		/* The context keeps a staging BO it could not release. */
	}
	session_free(s);
	return 0;
}
