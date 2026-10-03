/* The memory each ioctl of the Linux-file RPC touches (rt/lx_abi.h): the
 * argument block, and the ranges its pointers name, per command, as the
 * kernel code reaches them with copy_{from,to}_user. One table serves the
 * client library (which builds frames from it), the in-dext self-test and
 * the dext's command admission (mlg_lx_cmd_known).
 *
 * Built against the Linux uapi headers in both worlds: linuxu's verbatim
 * copies in the dext, the upstream include/uapi tree with Linux ioctl
 * encodings in the client library (MLG_LX_CLIENT_BUILD, libmlg_drm/compat). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef MLG_LX_CLIENT_BUILD
#include "mlg_uapi.h"
#else
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>
#include <uapi/linux/kfd_ioctl.h>
#endif
#include <rt/lx_abi.h>

struct span_list {
	struct mlg_lx_span *spans;
	uint32_t cap, count;
	int error;
};

static void add(struct span_list *l, uint64_t va, uint64_t size, uint32_t dir)
{
	if (l->error || !size)
		return;
	if (!va) {
		l->error = -MLG_LX_EFAULT;
		return;
	}
	if (size > MLG_LX_MAX_SEGMENT_BYTES || l->count == l->cap) {
		l->error = -MLG_LX_E2BIG;
		return;
	}
	l->spans[l->count++] = (struct mlg_lx_span){ .va = va, .size = size, .dir = dir };
}

/* Read @bytes of the caller's memory at @va (already described as IN). */
static void get(const struct span_list *l, uint64_t va, void *dst, size_t bytes)
{
	if (l->error || !va)
		memset(dst, 0, bytes);
	else
		memcpy(dst, (const void *)(uintptr_t)va, bytes);
}

static uint32_t ioc_dir(uint32_t cmd)
{
	uint32_t dir = 0;

	if (_IOC_DIR(cmd) & _IOC_WRITE)
		dir |= MLG_LX_SEG_IN;
	if (_IOC_DIR(cmd) & _IOC_READ)
		dir |= MLG_LX_SEG_OUT;
	return dir;
}

/* ---- commands ---- */

static int drm_core_known(uint32_t cmd)
{
	switch (cmd) {
	case DRM_IOCTL_VERSION:
	case DRM_IOCTL_GET_UNIQUE:
	case DRM_IOCTL_GET_CAP:
	case DRM_IOCTL_SET_CLIENT_CAP:
	case DRM_IOCTL_GEM_CLOSE:
	case DRM_IOCTL_PRIME_HANDLE_TO_FD:
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
	case DRM_IOCTL_SYNCOBJ_CREATE:
	case DRM_IOCTL_SYNCOBJ_DESTROY:
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
	case DRM_IOCTL_SYNCOBJ_WAIT:
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
	case DRM_IOCTL_SYNCOBJ_QUERY:
	case DRM_IOCTL_SYNCOBJ_TRANSFER:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_EVENTFD:
		return 1;
	default:
		return 0;
	}
}

static int amdgpu_known(uint32_t cmd)
{
	switch (cmd) {
	case DRM_IOCTL_AMDGPU_GEM_CREATE:
	case DRM_IOCTL_AMDGPU_CTX:
	case DRM_IOCTL_AMDGPU_VM:
	case DRM_IOCTL_AMDGPU_SCHED:
	case DRM_IOCTL_AMDGPU_BO_LIST:
	case DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE:
	case DRM_IOCTL_AMDGPU_GEM_MMAP:
	case DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE:
	case DRM_IOCTL_AMDGPU_CS:
	case DRM_IOCTL_AMDGPU_INFO:
	case DRM_IOCTL_AMDGPU_WAIT_CS:
	case DRM_IOCTL_AMDGPU_WAIT_FENCES:
	case DRM_IOCTL_AMDGPU_GEM_METADATA:
	case DRM_IOCTL_AMDGPU_GEM_VA:
	case DRM_IOCTL_AMDGPU_GEM_OP:
	case DRM_IOCTL_AMDGPU_GEM_USERPTR:
	case DRM_IOCTL_AMDGPU_USERQ:
	case DRM_IOCTL_AMDGPU_USERQ_SIGNAL:
	case DRM_IOCTL_AMDGPU_USERQ_WAIT:
	case DRM_IOCTL_AMDGPU_GEM_LIST_HANDLES:
		return 1;
	default:
		return 0;
	}
}

/* The KFD ioctls a compute runtime issues. Checkpoint/restore, the
 * debugger and SVM are not carried. kfd_ioctl dispatches by number alone
 * (whatever direction and size the encoding carries), so these are
 * refused by number too. */
static int kfd_known(uint32_t cmd)
{
	const uint32_t nr = _IOC_NR(cmd);

	if (_IOC_TYPE(cmd) != AMDKFD_IOCTL_BASE || nr < AMDKFD_COMMAND_START ||
	    nr >= AMDKFD_COMMAND_END)
		return 0;
	return nr != _IOC_NR(AMDKFD_IOC_SVM) && nr != _IOC_NR(AMDKFD_IOC_CRIU_OP) &&
	       nr != _IOC_NR(AMDKFD_IOC_DBG_TRAP);
}

int mlg_lx_cmd_known(uint32_t dev, uint32_t cmd)
{
	switch (dev) {
	case MLG_LX_DEV_RENDER:
		return drm_core_known(cmd) || amdgpu_known(cmd);
	case MLG_LX_DEV_KFD:
		return kfd_known(cmd);
	default:
		return 0;
	}
}

int mlg_lx_cmd_blocks(uint32_t dev, uint32_t cmd)
{
	if (dev == MLG_LX_DEV_KFD)
		return cmd == AMDKFD_IOC_WAIT_EVENTS;
	if (dev != MLG_LX_DEV_RENDER)
		return 0;
	switch (cmd) {
	case DRM_IOCTL_AMDGPU_WAIT_CS:
	case DRM_IOCTL_AMDGPU_WAIT_FENCES:
	case DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE:
	case DRM_IOCTL_SYNCOBJ_WAIT:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
		return 1;
	default:
		return 0;
	}
}

/* ---- nested ranges ---- */

static void drm_core_nested(struct span_list *l, uint32_t cmd, uint64_t arg,
			    uint64_t *timeout_va)
{
	switch (cmd) {
	case DRM_IOCTL_VERSION: {
		struct drm_version v;

		get(l, arg, &v, sizeof(v));
		/* The kernel copies min(len, its string) and reports the full
		 * length; a zero length asks for the length only. */
		add(l, (uint64_t)(uintptr_t)v.name, v.name_len, MLG_LX_SEG_OUT);
		add(l, (uint64_t)(uintptr_t)v.date, v.date_len, MLG_LX_SEG_OUT);
		add(l, (uint64_t)(uintptr_t)v.desc, v.desc_len, MLG_LX_SEG_OUT);
		break;
	}
	case DRM_IOCTL_GET_UNIQUE: {
		struct drm_unique u;

		get(l, arg, &u, sizeof(u));
		add(l, (uint64_t)(uintptr_t)u.unique, u.unique_len, MLG_LX_SEG_OUT);
		break;
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait w;

		get(l, arg, &w, sizeof(w));
		add(l, w.handles, (uint64_t)w.count_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		*timeout_va = arg + offsetof(struct drm_syncobj_wait, timeout_nsec);
		break;
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
		struct drm_syncobj_timeline_wait w;

		get(l, arg, &w, sizeof(w));
		add(l, w.handles, (uint64_t)w.count_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		add(l, w.points, (uint64_t)w.count_handles * sizeof(uint64_t), MLG_LX_SEG_IN);
		*timeout_va = arg + offsetof(struct drm_syncobj_timeline_wait, timeout_nsec);
		break;
	}
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array a;

		get(l, arg, &a, sizeof(a));
		add(l, a.handles, (uint64_t)a.count_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		break;
	}
	case DRM_IOCTL_SYNCOBJ_QUERY:
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: {
		struct drm_syncobj_timeline_array a;

		get(l, arg, &a, sizeof(a));
		add(l, a.handles, (uint64_t)a.count_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		add(l, a.points, (uint64_t)a.count_handles * sizeof(uint64_t),
		    cmd == DRM_IOCTL_SYNCOBJ_QUERY ? MLG_LX_SEG_OUT : MLG_LX_SEG_IN);
		break;
	}
	default:
		break;
	}
}

static void cs_nested(struct span_list *l, uint64_t arg)
{
	union drm_amdgpu_cs cs;
	uint64_t *chunks;

	get(l, arg, &cs, sizeof(cs));
	add(l, cs.in.chunks, (uint64_t)cs.in.num_chunks * sizeof(uint64_t), MLG_LX_SEG_IN);
	if (l->error || !cs.in.num_chunks)
		return;
	chunks = (uint64_t *)(uintptr_t)cs.in.chunks;
	for (uint32_t i = 0; i < cs.in.num_chunks && !l->error; ++i) {
		struct drm_amdgpu_cs_chunk chunk;
		uint64_t pointer;

		get(l, (uint64_t)(uintptr_t)&chunks[i], &pointer, sizeof(pointer));
		add(l, pointer, sizeof(chunk), MLG_LX_SEG_IN);
		get(l, pointer, &chunk, sizeof(chunk));
		add(l, chunk.chunk_data, (uint64_t)chunk.length_dw * 4, MLG_LX_SEG_IN);
		if (chunk.chunk_id == AMDGPU_CHUNK_ID_BO_HANDLES &&
		    (uint64_t)chunk.length_dw * 4 >= sizeof(struct drm_amdgpu_bo_list_in)) {
			struct drm_amdgpu_bo_list_in in;

			get(l, chunk.chunk_data, &in, sizeof(in));
			add(l, in.bo_info_ptr, (uint64_t)in.bo_number * in.bo_info_size,
			    MLG_LX_SEG_IN);
		}
	}
}

static void amdgpu_nested(struct span_list *l, uint32_t cmd, uint64_t arg,
			  uint64_t *timeout_va)
{
	switch (cmd) {
	case DRM_IOCTL_AMDGPU_INFO: {
		struct drm_amdgpu_info info;

		get(l, arg, &info, sizeof(info));
		add(l, info.return_pointer, info.return_size, MLG_LX_SEG_OUT);
		break;
	}
	case DRM_IOCTL_AMDGPU_CS:
		cs_nested(l, arg);
		break;
	case DRM_IOCTL_AMDGPU_BO_LIST: {
		union drm_amdgpu_bo_list list;

		get(l, arg, &list, sizeof(list));
		add(l, list.in.bo_info_ptr, (uint64_t)list.in.bo_number * list.in.bo_info_size,
		    MLG_LX_SEG_IN);
		break;
	}
	case DRM_IOCTL_AMDGPU_WAIT_CS:
		*timeout_va = arg + offsetof(union drm_amdgpu_wait_cs, in.timeout);
		break;
	case DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE:
		*timeout_va = arg + offsetof(union drm_amdgpu_gem_wait_idle, in.timeout);
		break;
	case DRM_IOCTL_AMDGPU_WAIT_FENCES: {
		union drm_amdgpu_wait_fences w;

		get(l, arg, &w, sizeof(w));
		add(l, w.in.fences, (uint64_t)w.in.fence_count * sizeof(struct drm_amdgpu_fence),
		    MLG_LX_SEG_IN);
		*timeout_va = arg + offsetof(union drm_amdgpu_wait_fences, in.timeout_ns);
		break;
	}
	case DRM_IOCTL_AMDGPU_GEM_OP: {
		struct drm_amdgpu_gem_op op;

		get(l, arg, &op, sizeof(op));
		if (op.op == AMDGPU_GEM_OP_GET_GEM_CREATE_INFO)
			add(l, op.value, sizeof(struct drm_amdgpu_gem_create_in), MLG_LX_SEG_OUT);
		else if (op.op == AMDGPU_GEM_OP_GET_MAPPING_INFO)
			add(l, op.value,
			    (uint64_t)op.num_entries * sizeof(struct drm_amdgpu_gem_vm_entry),
			    MLG_LX_SEG_OUT);
		break;
	}
	case DRM_IOCTL_AMDGPU_GEM_LIST_HANDLES: {
		struct drm_amdgpu_gem_list_handles h;

		get(l, arg, &h, sizeof(h));
		add(l, h.entries,
		    (uint64_t)h.num_entries * sizeof(struct drm_amdgpu_gem_list_handles_entry),
		    MLG_LX_SEG_OUT);
		break;
	}
	case DRM_IOCTL_AMDGPU_GEM_VA: {
		struct drm_amdgpu_gem_va va;

		get(l, arg, &va, sizeof(va));
		add(l, va.input_fence_syncobj_handles,
		    (uint64_t)va.num_syncobj_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		break;
	}
	case DRM_IOCTL_AMDGPU_USERQ: {
		union drm_amdgpu_userq q;

		get(l, arg, &q, sizeof(q));
		if (q.in.op == AMDGPU_USERQ_OP_CREATE)
			add(l, q.in.mqd, q.in.mqd_size, MLG_LX_SEG_IN);
		break;
	}
	case DRM_IOCTL_AMDGPU_USERQ_SIGNAL: {
		struct drm_amdgpu_userq_signal s;

		get(l, arg, &s, sizeof(s));
		add(l, s.syncobj_handles, (uint64_t)s.num_syncobj_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		add(l, s.bo_read_handles, (uint64_t)s.num_bo_read_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		add(l, s.bo_write_handles, (uint64_t)s.num_bo_write_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		break;
	}
	case DRM_IOCTL_AMDGPU_USERQ_WAIT: {
		struct drm_amdgpu_userq_wait w;

		get(l, arg, &w, sizeof(w));
		add(l, w.syncobj_handles, (uint64_t)w.num_syncobj_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		add(l, w.syncobj_timeline_handles,
		    (uint64_t)w.num_syncobj_timeline_handles * sizeof(uint32_t), MLG_LX_SEG_IN);
		add(l, w.syncobj_timeline_points,
		    (uint64_t)w.num_syncobj_timeline_handles * sizeof(uint64_t), MLG_LX_SEG_IN);
		add(l, w.bo_read_handles, (uint64_t)w.num_bo_read_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		add(l, w.bo_write_handles, (uint64_t)w.num_bo_write_handles * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		add(l, w.out_fences,
		    (uint64_t)w.num_fences * sizeof(struct drm_amdgpu_userq_fence_info),
		    MLG_LX_SEG_OUT);
		break;
	}
	default:
		break;
	}
}

static void kfd_nested(struct span_list *l, uint32_t cmd, uint64_t arg)
{
	switch (cmd) {
	case AMDKFD_IOC_GET_PROCESS_APERTURES_NEW: {
		struct kfd_ioctl_get_process_apertures_new_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.kfd_process_device_apertures_ptr,
		    (uint64_t)a.num_of_nodes * sizeof(struct kfd_process_device_apertures),
		    MLG_LX_SEG_OUT);
		break;
	}
	case AMDKFD_IOC_WAIT_EVENTS: {
		struct kfd_ioctl_wait_events_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.events_ptr, (uint64_t)a.num_events * sizeof(struct kfd_event_data),
		    MLG_LX_SEG_INOUT);
		break;
	}
	case AMDKFD_IOC_MAP_MEMORY_TO_GPU:
	case AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU: {
		struct kfd_ioctl_map_memory_to_gpu_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.device_ids_array_ptr, (uint64_t)a.n_devices * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		break;
	}
	case AMDKFD_IOC_GET_TILE_CONFIG: {
		struct kfd_ioctl_get_tile_config_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.tile_config_ptr, (uint64_t)a.num_tile_configs * sizeof(uint32_t),
		    MLG_LX_SEG_OUT);
		add(l, a.macro_tile_config_ptr,
		    (uint64_t)a.num_macro_tile_configs * sizeof(uint32_t), MLG_LX_SEG_OUT);
		break;
	}
	case AMDKFD_IOC_SET_CU_MASK: {
		struct kfd_ioctl_set_cu_mask_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.cu_mask_ptr, (uint64_t)(a.num_cu_mask / 32) * sizeof(uint32_t),
		    MLG_LX_SEG_IN);
		break;
	}
	case AMDKFD_IOC_GET_DMABUF_INFO: {
		struct kfd_ioctl_get_dmabuf_info_args a;

		get(l, arg, &a, sizeof(a));
		add(l, a.metadata_ptr, a.metadata_size, MLG_LX_SEG_OUT);
		break;
	}
	default:
		break;
	}
}

int mlg_lx_describe(uint32_t dev, uint32_t cmd, uint64_t arg,
		    struct mlg_lx_span *spans, uint32_t cap, uint32_t *count,
		    uint64_t *timeout_va)
{
	struct span_list l = { .spans = spans, .cap = cap };
	uint64_t timeout = 0;

	if (!count || !timeout_va || (cap && !spans))
		return -MLG_LX_EINVAL;
	*count = 0;
	*timeout_va = 0;
	if (!mlg_lx_cmd_known(dev, cmd))
		return -MLG_LX_ENOTTY;
	/* The argument block, as the ioctl encoding sizes it. */
	if (_IOC_DIR(cmd) != _IOC_NONE && _IOC_SIZE(cmd)) {
		if (!arg)
			return -MLG_LX_EFAULT;
		add(&l, arg, _IOC_SIZE(cmd), ioc_dir(cmd));
		if (dev == MLG_LX_DEV_KFD)
			kfd_nested(&l, cmd, arg);
		else if (drm_core_known(cmd))
			drm_core_nested(&l, cmd, arg, &timeout);
		else
			amdgpu_nested(&l, cmd, arg, &timeout);
	}
	if (l.error)
		return l.error;
	*count = l.count;
	*timeout_va = timeout;
	return 0;
}
