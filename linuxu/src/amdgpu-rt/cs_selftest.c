/* Kernel-queue command submission self-test (rt/cs_selftest.h): a render
 * node client, as Mesa RADV is one, running through the Linux-file
 * transport inside the dext. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/amdgpu_drm.h>
#include <rt/cs_selftest.h>
#include <rt/lx_files.h>

#include "amdgpu.h"
#include "amdgpu_ring.h"
#include "nvd.h"

/* GTT data buffer layout. */
#define ST_BO_BYTES		(64u << 10)
#define ST_COMPUTE_OFF		0u
#define ST_COPY_OFF		4096u	/* the VRAM copy lands here */
#define ST_COPY_BYTES		2048u
#define ST_MOVE_GTT_OFF		8192u	/* the copy made from GTT lands here */
#define ST_MOVE_VRAM_OFF	12288u	/* ... and the one after the move back */
/* The user fence: AMDGPU_CS wants a buffer of exactly one page for it. */
#define ST_USER_FENCE_OFF	64u
/* IB buffer layout (IB start alignments are at most 256 bytes). */
#define ST_IB_COMPUTE		0u
#define ST_IB_FILL		4096u
#define ST_IB_COPY		8192u
#define ST_IB_MOVE_GTT		12288u
#define ST_IB_MOVE_VRAM		16384u
#define ST_IB_SLOT_BYTES	4096u
/* VRAM buffer: the compute value at 0, the fill from ST_FILL_OFF. */
#define ST_FILL_OFF		1024u
#define ST_FILL_BYTES		1024u

#define ST_COMPUTE_GTT_VALUE	0xc0de0001u
#define ST_COMPUTE_VRAM_VALUE	0xc0de0002u
#define ST_FILL_VALUE		0x5eed5eedu

enum { BO_DATA, BO_IB, BO_FENCE, BO_VRAM, BO_COUNT };

struct st {
	struct rt_cs_selftest_result *res;
	struct amdgpu_device *adev;
	struct rt_lx_client *c;
	int fd;
	uint32_t ctx_id;
	bool ctx;
	uint32_t syncobj[2];
	uint32_t bo[BO_COUNT];
	uint64_t va[BO_COUNT];
	bool mapped_va[BO_COUNT];
	uint64_t map_type[BO_COUNT];
	uint8_t *cpu[BO_COUNT];
	struct drm_amdgpu_info_device dev;
	struct drm_amdgpu_info_hw_ip compute, dma;
	/* The test's own memory for ioctl arguments: one heap block, so the
	 * calls' argument pages are real client memory of this process. */
	union {
		struct drm_version version;
		struct drm_amdgpu_info info;
		union drm_amdgpu_ctx ctx;
		struct drm_syncobj_create syncobj_create;
		struct drm_syncobj_destroy syncobj_destroy;
		struct drm_syncobj_wait syncobj_wait;
		union drm_amdgpu_gem_create gem_create;
		union drm_amdgpu_gem_mmap gem_mmap;
		struct drm_amdgpu_gem_op gem_op;
		struct drm_amdgpu_gem_va gem_va;
		struct drm_gem_close gem_close;
		union drm_amdgpu_cs cs;
		union drm_amdgpu_wait_cs wait_cs;
	} arg;
	char name[32], date[32], desc[64];
	uint32_t handles[4];
	struct drm_amdgpu_bo_list_entry bo_list[BO_COUNT];
	struct drm_amdgpu_bo_list_in bo_list_in;
	struct drm_amdgpu_cs_chunk_ib chunk_ib;
	struct drm_amdgpu_cs_chunk_fence chunk_fence;
	struct drm_amdgpu_cs_chunk_sem sem_out, sem_in;
	struct drm_amdgpu_cs_chunk chunks[5];
	uint64_t chunk_ptrs[5];
	/* Submissions made, for the check that none is still running. */
	struct { uint32_t ip_type; uint64_t seq; } subs[8];
	unsigned int nsubs;
	struct rt_cs_selftest_result parked_res;
	/* An async wait's completion. */
	struct completion done;
	int64_t async_result;
	uint8_t async_reply[256];
	size_t async_reply_bytes;
};

/* ---- the client side of one ioctl ---- */

static long build_frame(uint32_t cmd, void *arg, void **frame, size_t *bytes, uint64_t *out)
{
	struct mlg_lx_span *spans;
	uint64_t timeout_va = 0;
	uint32_t n = 0;
	long len;
	int r;

	spans = kvcalloc(MLG_LX_DESCRIBE_MAX, sizeof(*spans), GFP_KERNEL);
	if (!spans)
		return -ENOMEM;
	r = mlg_lx_describe(MLG_LX_DEV_RENDER, cmd, (uint64_t)(uintptr_t)arg, spans,
			    MLG_LX_DESCRIBE_MAX, &n, &timeout_va);
	if (r) {
		kvfree(spans);
		return r;
	}
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va,
			    ktime_get_ns(), NULL, 0, out);
	if (len > 0) {
		*frame = kvmalloc(len, GFP_KERNEL);
		if (!*frame)
			len = -ENOMEM;
		else
			len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va,
					    ktime_get_ns(), *frame, len, out);
	}
	kvfree(spans);
	if (len < 0) {
		kvfree(*frame);
		*frame = NULL;
		return len;
	}
	*bytes = len;
	return 0;
}

static long st_ioctl(struct st *s, uint32_t cmd, void *arg)
{
	void *frame = NULL, *rep = NULL;
	size_t bytes = 0, rep_bytes = 0;
	uint64_t out = 0;
	int64_t result = 0;
	long r;

	r = build_frame(cmd, arg, &frame, &bytes, &out);
	if (r)
		return r;
	rep = kvmalloc(mlg_lx_reply_bytes(out), GFP_KERNEL);
	if (!rep) {
		kvfree(frame);
		return -ENOMEM;
	}
	r = rt_lx_ioctl(s->c, s->fd, cmd, frame, bytes, rep, mlg_lx_reply_bytes(out),
			&rep_bytes, &result);
	if (!r)
		r = mlg_lx_apply_reply(frame, bytes, rep, rep_bytes, NULL) ? -EPROTO : result;
	kvfree(rep);
	kvfree(frame);
	return r;
}

static int async_done(void *ctx, uint64_t token, int64_t result, const void *rbuf,
		      size_t reply_bytes)
{
	struct st *s = ctx;

	(void)token;
	s->async_result = result;
	s->async_reply_bytes = reply_bytes <= sizeof(s->async_reply) ? reply_bytes : 0;
	if (s->async_reply_bytes)
		memcpy(s->async_reply, rbuf, reply_bytes);
	complete(&s->done);
	return 1;
}

/* A wait run as the async RPC runs it: on a worker of the process. */
static long st_ioctl_async(struct st *s, uint32_t cmd, void *arg)
{
	void *frame = NULL;
	size_t bytes = 0;
	uint64_t token = 0;
	long r;

	r = build_frame(cmd, arg, &frame, &bytes, NULL);
	if (r)
		return r;
	reinit_completion(&s->done);
	r = rt_lx_ioctl_async(s->c, s->fd, cmd, frame, bytes, async_done, s, &token);
	if (!r) {
		/* The call itself is bounded by its deadline; this only guards
		 * against a worker that never ran. */
		if (!wait_for_completion_timeout(&s->done,
				msecs_to_jiffies(RT_CS_SELFTEST_WAIT_MS * 4)))
			r = -ETIME;
		else if (!s->async_reply_bytes ||
			 mlg_lx_apply_reply(frame, bytes, s->async_reply, s->async_reply_bytes, NULL))
			r = -EPROTO;
		else
			r = s->async_result;
	}
	kvfree(frame);
	return r;
}

static uint64_t deadline(void)
{
	return ktime_get_ns() + (uint64_t)RT_CS_SELFTEST_WAIT_MS * 1000000ull;
}

/* ---- steps ---- */

static int step_version(struct st *s)
{
	memset(&s->arg.version, 0, sizeof(s->arg.version));
	s->arg.version.name = s->name;
	s->arg.version.name_len = sizeof(s->name) - 1;
	s->arg.version.date = s->date;
	s->arg.version.date_len = sizeof(s->date) - 1;
	s->arg.version.desc = s->desc;
	s->arg.version.desc_len = sizeof(s->desc) - 1;
	int r = (int)st_ioctl(s, DRM_IOCTL_VERSION, &s->arg.version);

	if (r)
		return r;
	return strcmp(s->name, "amdgpu") ? RT_CS_MISMATCH : 0;
}

static int query(struct st *s, uint32_t what, void *out, uint32_t size, uint32_t ip_type)
{
	memset(&s->arg.info, 0, sizeof(s->arg.info));
	s->arg.info.return_pointer = (uint64_t)(uintptr_t)out;
	s->arg.info.return_size = size;
	s->arg.info.query = what;
	s->arg.info.query_hw_ip.type = ip_type;
	return (int)st_ioctl(s, DRM_IOCTL_AMDGPU_INFO, &s->arg.info);
}

static int step_dev_info(struct st *s)
{
	int r = query(s, AMDGPU_INFO_DEV_INFO, &s->dev, sizeof(s->dev), 0);

	if (r)
		return r;
	s->res->family = s->dev.family;
	s->res->chip_external_rev = s->dev.external_rev;
	s->res->device_id = s->dev.device_id;
	s->res->num_shader_engines = s->dev.num_shader_engines;
	return 0;
}

static int step_hw_ip(struct st *s)
{
	int r = query(s, AMDGPU_INFO_HW_IP_INFO, &s->compute, sizeof(s->compute),
		      AMDGPU_HW_IP_COMPUTE);

	if (!r)
		r = query(s, AMDGPU_INFO_HW_IP_INFO, &s->dma, sizeof(s->dma), AMDGPU_HW_IP_DMA);
	s->res->compute_rings = s->compute.available_rings;
	s->res->sdma_rings = s->dma.available_rings;
	return r;
}

static int step_ctx(struct st *s)
{
	int r;

	memset(&s->arg.ctx, 0, sizeof(s->arg.ctx));
	s->arg.ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
	s->arg.ctx.in.priority = AMDGPU_CTX_PRIORITY_NORMAL;
	r = (int)st_ioctl(s, DRM_IOCTL_AMDGPU_CTX, &s->arg.ctx);
	if (!r) {
		s->ctx_id = s->arg.ctx.out.alloc.ctx_id;
		s->ctx = true;
	}
	return r;
}

static int step_syncobj(struct st *s)
{
	for (int i = 0; i < 2; ++i) {
		int r;

		memset(&s->arg.syncobj_create, 0, sizeof(s->arg.syncobj_create));
		r = (int)st_ioctl(s, DRM_IOCTL_SYNCOBJ_CREATE, &s->arg.syncobj_create);
		if (r)
			return r;
		s->syncobj[i] = s->arg.syncobj_create.handle;
	}
	return 0;
}

static uint64_t bo_bytes(int i)
{
	return i == BO_FENCE ? PAGE_SIZE : ST_BO_BYTES;
}

static int step_gem_create(struct st *s)
{
	static const uint32_t domain[BO_COUNT] = {
		AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_DOMAIN_GTT,
		AMDGPU_GEM_DOMAIN_VRAM,
	};
	static const uint64_t flags[BO_COUNT] = {
		AMDGPU_GEM_CREATE_CPU_GTT_USWC, AMDGPU_GEM_CREATE_CPU_GTT_USWC, 0,
		AMDGPU_GEM_CREATE_NO_CPU_ACCESS | AMDGPU_GEM_CREATE_VRAM_CLEARED,
	};

	for (int i = 0; i < BO_COUNT; ++i) {
		int r;

		memset(&s->arg.gem_create, 0, sizeof(s->arg.gem_create));
		s->arg.gem_create.in.bo_size = bo_bytes(i);
		s->arg.gem_create.in.alignment = PAGE_SIZE;
		s->arg.gem_create.in.domains = domain[i];
		s->arg.gem_create.in.domain_flags = flags[i];
		r = (int)st_ioctl(s, DRM_IOCTL_AMDGPU_GEM_CREATE, &s->arg.gem_create);
		if (r)
			return r;
		s->bo[i] = s->arg.gem_create.out.handle;
	}
	return 0;
}

static int gem_va(struct st *s, int i, uint32_t op)
{
	memset(&s->arg.gem_va, 0, sizeof(s->arg.gem_va));
	s->arg.gem_va.handle = s->bo[i];
	s->arg.gem_va.operation = op;
	s->arg.gem_va.flags = op == AMDGPU_VA_OP_MAP ?
		AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE : 0;
	s->arg.gem_va.va_address = s->va[i];
	s->arg.gem_va.map_size = bo_bytes(i);
	return (int)st_ioctl(s, DRM_IOCTL_AMDGPU_GEM_VA, &s->arg.gem_va);
}

static int step_gem_va(struct st *s)
{
	/* Above the reserved bottom of the range, 2 MiB apart. */
	const uint64_t step = 2ull << 20;
	uint64_t align = s->dev.virtual_address_alignment > step ?
		s->dev.virtual_address_alignment : step;
	uint64_t base = ALIGN(s->dev.virtual_address_offset, align) + step;

	if (!s->dev.virtual_address_max || base + BO_COUNT * step > s->dev.virtual_address_max)
		return -ENOSPC;
	s->res->va_start = base;
	s->res->va_end = base + BO_COUNT * step;
	for (int i = 0; i < BO_COUNT; ++i) {
		int r;

		s->va[i] = base + i * step;
		r = gem_va(s, i, AMDGPU_VA_OP_MAP);
		if (r)
			return r;
		s->mapped_va[i] = true;
	}
	return 0;
}

static int step_gem_mmap(struct st *s)
{
	for (int i = BO_DATA; i <= BO_FENCE; ++i) {
		struct rt_lx_map_info info;
		uint64_t contiguous = 0;
		int r;

		memset(&s->arg.gem_mmap, 0, sizeof(s->arg.gem_mmap));
		s->arg.gem_mmap.in.handle = s->bo[i];
		r = (int)st_ioctl(s, DRM_IOCTL_AMDGPU_GEM_MMAP, &s->arg.gem_mmap);
		if (r)
			return r;
		r = rt_lx_mmap(s->c, s->fd, s->arg.gem_mmap.out.addr_ptr, bo_bytes(i),
			       MLG_LX_PROT_READ | MLG_LX_PROT_WRITE, MLG_LX_MAP_SHARED, &info);
		if (r)
			return r;
		s->map_type[i] = info.type;
		/* The test reads and writes the pages directly, so it needs
		 * them in one run; a client maps them whatever they are. */
		s->cpu[i] = rt_lx_map_cpu(s->c, info.type, 0, &contiguous);
		if (!s->cpu[i] || contiguous < bo_bytes(i))
			return -EFAULT;
		memset(s->cpu[i], 0, bo_bytes(i));
	}
	return 0;
}

static void chunk(struct st *s, uint32_t i, uint32_t id, const void *data, uint32_t bytes)
{
	s->chunks[i].chunk_id = id;
	s->chunks[i].length_dw = bytes / 4;
	s->chunks[i].chunk_data = (uint64_t)(uintptr_t)data;
	s->chunk_ptrs[i] = (uint64_t)(uintptr_t)&s->chunks[i];
}

/* AMDGPU_CS of one IB with every test buffer in its BO list. */
static int submit(struct st *s, uint32_t ip_type, uint64_t ib_va, uint32_t ib_bytes,
		  bool user_fence, int signal, int wait_for, uint64_t *seq)
{
	uint32_t n = 0;
	int r;

	for (int i = 0; i < BO_COUNT; ++i)
		s->bo_list[i] = (struct drm_amdgpu_bo_list_entry){ .bo_handle = s->bo[i] };
	s->bo_list_in = (struct drm_amdgpu_bo_list_in){
		.operation = ~0u, .list_handle = ~0u, .bo_number = BO_COUNT,
		.bo_info_size = sizeof(s->bo_list[0]),
		.bo_info_ptr = (uint64_t)(uintptr_t)s->bo_list,
	};
	s->chunk_ib = (struct drm_amdgpu_cs_chunk_ib){
		.ip_type = ip_type, .va_start = ib_va, .ib_bytes = ib_bytes,
	};
	chunk(s, n++, AMDGPU_CHUNK_ID_BO_HANDLES, &s->bo_list_in, sizeof(s->bo_list_in));
	chunk(s, n++, AMDGPU_CHUNK_ID_IB, &s->chunk_ib, sizeof(s->chunk_ib));
	if (user_fence) {
		s->chunk_fence = (struct drm_amdgpu_cs_chunk_fence){
			.handle = s->bo[BO_FENCE], .offset = ST_USER_FENCE_OFF,
		};
		chunk(s, n++, AMDGPU_CHUNK_ID_FENCE, &s->chunk_fence, sizeof(s->chunk_fence));
	}
	if (wait_for >= 0) {
		s->sem_in.handle = s->syncobj[wait_for];
		chunk(s, n++, AMDGPU_CHUNK_ID_SYNCOBJ_IN, &s->sem_in, sizeof(s->sem_in));
	}
	if (signal >= 0) {
		s->sem_out.handle = s->syncobj[signal];
		chunk(s, n++, AMDGPU_CHUNK_ID_SYNCOBJ_OUT, &s->sem_out, sizeof(s->sem_out));
	}
	memset(&s->arg.cs, 0, sizeof(s->arg.cs));
	s->arg.cs.in.ctx_id = s->ctx_id;
	s->arg.cs.in.num_chunks = n;
	s->arg.cs.in.chunks = (uint64_t)(uintptr_t)s->chunk_ptrs;
	r = (int)st_ioctl(s, DRM_IOCTL_AMDGPU_CS, &s->arg.cs);
	if (!r) {
		*seq = s->arg.cs.out.handle;
		if (s->nsubs < ARRAY_SIZE(s->subs)) {
			s->subs[s->nsubs].ip_type = ip_type;
			s->subs[s->nsubs++].seq = *seq;
		}
	}
	return r;
}

static int wait_cs(struct st *s, uint32_t ip_type, uint64_t seq)
{
	int r;

	memset(&s->arg.wait_cs, 0, sizeof(s->arg.wait_cs));
	s->arg.wait_cs.in.handle = seq;
	s->arg.wait_cs.in.timeout = deadline();
	s->arg.wait_cs.in.ip_type = ip_type;
	s->arg.wait_cs.in.ctx_id = s->ctx_id;
	r = (int)st_ioctl_async(s, DRM_IOCTL_AMDGPU_WAIT_CS, &s->arg.wait_cs);
	if (r)
		return r;
	/* out.status is nonzero when the deadline passed first. */
	return s->arg.wait_cs.out.status ? -ETIME : 0;
}

static int wait_syncobj(struct st *s, int which)
{
	s->handles[0] = s->syncobj[which];
	memset(&s->arg.syncobj_wait, 0, sizeof(s->arg.syncobj_wait));
	s->arg.syncobj_wait.handles = (uint64_t)(uintptr_t)s->handles;
	s->arg.syncobj_wait.count_handles = 1;
	s->arg.syncobj_wait.timeout_nsec = (int64_t)deadline();
	s->arg.syncobj_wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	return (int)st_ioctl(s, DRM_IOCTL_SYNCOBJ_WAIT, &s->arg.syncobj_wait);
}

static int compute_engine(struct st *s)
{
	return s->compute.available_rings && s->adev->gfx.num_compute_rings;
}

static int sdma_engine(struct st *s)
{
	return s->dma.available_rings && s->adev->mman.buffer_funcs &&
	       s->adev->mman.buffer_funcs_ring;
}

static uint32_t *ib_at(struct st *s, uint32_t offset)
{
	return (uint32_t *)(void *)(s->cpu[BO_IB] + offset);
}

/* PM4 WRITE_DATA of one dword to memory, confirmed before the next packet. */
static uint32_t write_data(uint32_t *ib, uint32_t n, uint64_t va, uint32_t value)
{
	ib[n++] = PACKET3(PACKET3_WRITE_DATA, 3);
	ib[n++] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
	ib[n++] = lower_32_bits(va);
	ib[n++] = upper_32_bits(va);
	ib[n++] = value;
	return n;
}

static int step_compute_cs(struct st *s)
{
	uint32_t *ib = ib_at(s, ST_IB_COMPUTE);
	uint32_t n = 0, align = s->compute.ib_size_alignment / 4;
	uint32_t nop = s->adev->gfx.compute_ring[0].funcs->nop;

	n = write_data(ib, n, s->va[BO_DATA] + ST_COMPUTE_OFF, ST_COMPUTE_GTT_VALUE);
	n = write_data(ib, n, s->va[BO_VRAM], ST_COMPUTE_VRAM_VALUE);
	while (align > 1 && n % align)
		ib[n++] = nop;
	s->res->compute_ns = ktime_get_ns();
	return submit(s, AMDGPU_HW_IP_COMPUTE, s->va[BO_IB] + ST_IB_COMPUTE, n * 4, true,
		      0, -1, &s->res->compute_seq);
}

static int step_compute_wait(struct st *s)
{
	int r = wait_cs(s, AMDGPU_HW_IP_COMPUTE, s->res->compute_seq);

	s->res->compute_ns = ktime_get_ns() - s->res->compute_ns;
	return r;
}

static int step_compute_result(struct st *s)
{
	uint64_t fence;

	s->res->compute_value = *(volatile uint32_t *)(void *)(s->cpu[BO_DATA] + ST_COMPUTE_OFF);
	memcpy(&fence, s->cpu[BO_FENCE] + ST_USER_FENCE_OFF, sizeof(fence));
	s->res->user_fence = (uint32_t)fence;
	return s->res->compute_value == ST_COMPUTE_GTT_VALUE && fence == s->res->compute_seq ?
		0 : RT_CS_MISMATCH;
}

/* An SDMA IB from the device's buffer functions, padded by its ring. */
static uint32_t sdma_ib(struct st *s, uint32_t offset, bool fill)
{
	struct amdgpu_device *adev = s->adev;
	struct amdgpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct amdgpu_ib ib = {
		.ptr = ib_at(s, offset), .gpu_addr = s->va[BO_IB] + offset,
	};

	if (fill)
		amdgpu_emit_fill_buffer(adev, &ib, ST_FILL_VALUE, s->va[BO_VRAM] + ST_FILL_OFF,
					ST_FILL_BYTES);
	else
		amdgpu_emit_copy_buffer(adev, &ib, s->va[BO_VRAM], s->va[BO_DATA] + ST_COPY_OFF,
					ST_COPY_BYTES, 0);
	ring->funcs->pad_ib(ring, &ib);
	return ib.length_dw <= ST_IB_SLOT_BYTES / 4 ? ib.length_dw * 4 : 0;
}

static int step_sdma_fill(struct st *s)
{
	uint64_t seq;
	uint32_t bytes;

	if (s->adev->mman.buffer_funcs->fill_max_bytes < ST_FILL_BYTES ||
	    s->adev->mman.buffer_funcs->copy_max_bytes < ST_COPY_BYTES)
		return RT_CS_SKIPPED;
	bytes = sdma_ib(s, ST_IB_FILL, true);
	if (!bytes)
		return -E2BIG;
	return submit(s, AMDGPU_HW_IP_DMA, s->va[BO_IB] + ST_IB_FILL, bytes, false, 1, -1, &seq);
}

static int step_sdma_copy(struct st *s)
{
	uint32_t bytes = sdma_ib(s, ST_IB_COPY, false);

	if (!bytes)
		return -E2BIG;
	s->res->sdma_ns = ktime_get_ns();
	/* Ordered after the fill by its syncobj, explicitly. */
	return submit(s, AMDGPU_HW_IP_DMA, s->va[BO_IB] + ST_IB_COPY, bytes, false, -1, 1,
		      &s->res->sdma_seq);
}

static int step_sdma_wait(struct st *s)
{
	int r = wait_cs(s, AMDGPU_HW_IP_DMA, s->res->sdma_seq);

	s->res->sdma_ns = ktime_get_ns() - s->res->sdma_ns;
	return r;
}

/* A copy of the VRAM buffer's start: the compute value, then zeros (the
 * buffer was created cleared), then the fill. */
static int check_vram_copy(const uint8_t *copy)
{
	uint32_t word;
	int r = 0;

	memcpy(&word, copy, 4);
	if (word != ST_COMPUTE_VRAM_VALUE)
		r = RT_CS_MISMATCH;
	for (uint32_t off = 4; off < ST_COPY_BYTES; off += 4) {
		memcpy(&word, copy + off, 4);
		if (word != (off >= ST_FILL_OFF && off < ST_FILL_OFF + ST_FILL_BYTES ?
			     ST_FILL_VALUE : 0))
			r = RT_CS_MISMATCH;
	}
	return r;
}

static int step_sdma_result(struct st *s)
{
	const uint8_t *copy = s->cpu[BO_DATA] + ST_COPY_OFF;

	memcpy(&s->res->vram_value, copy, 4);
	memcpy(&s->res->fill_value, copy + ST_FILL_OFF, 4);
	return check_vram_copy(copy);
}

/* Move the VRAM buffer to @domain as a client's submission moves it: set
 * its placement (AMDGPU_GEM_OP SET_PLACEMENT), then submit an SDMA copy of
 * it into the data buffer; AMDGPU_CS validates the buffer list, so TTM
 * moves the buffer (amdgpu_move_blit) before the copy runs, and the VM
 * maps it where it went. Into GTT the move's destination goes through a
 * GART transfer window, as an eviction's does: SDMA uploads the window's
 * PTEs, flushes VMID 0 and copies. The wait is bounded; a move or copy
 * that never completes fails the step with -ETIME and parks the test. */
static int step_ttm_move(struct st *s, uint32_t domain, uint32_t ib_off, uint32_t data_off,
			 uint64_t *moved, uint64_t *ns, uint32_t *value)
{
	struct amdgpu_device *adev = s->adev;
	struct amdgpu_ring *ring = adev->mman.buffer_funcs_ring;
	struct amdgpu_ib ib = { .ptr = ib_at(s, ib_off), .gpu_addr = s->va[BO_IB] + ib_off };
	const uint64_t before = atomic64_read(&adev->num_bytes_moved);
	uint64_t seq = 0;
	int r;

	*ns = ktime_get_ns();
	memset(&s->arg.gem_op, 0, sizeof(s->arg.gem_op));
	s->arg.gem_op.handle = s->bo[BO_VRAM];
	s->arg.gem_op.op = AMDGPU_GEM_OP_SET_PLACEMENT;
	s->arg.gem_op.value = domain;
	r = (int)st_ioctl(s, DRM_IOCTL_AMDGPU_GEM_OP, &s->arg.gem_op);
	if (r)
		return r;
	memset(s->cpu[BO_DATA] + data_off, 0, ST_COPY_BYTES);
	amdgpu_emit_copy_buffer(adev, &ib, s->va[BO_VRAM], s->va[BO_DATA] + data_off,
				ST_COPY_BYTES, 0);
	ring->funcs->pad_ib(ring, &ib);
	if (ib.length_dw > ST_IB_SLOT_BYTES / 4)
		return -E2BIG;
	r = submit(s, AMDGPU_HW_IP_DMA, s->va[BO_IB] + ib_off, ib.length_dw * 4, false, -1, -1,
		   &seq);
	/* Other clients' moves count too: this is at least the buffer's. */
	*moved = atomic64_read(&adev->num_bytes_moved) - before;
	if (!r)
		r = wait_cs(s, AMDGPU_HW_IP_DMA, seq);
	*ns = ktime_get_ns() - *ns;
	if (r)
		return r;
	memcpy(value, s->cpu[BO_DATA] + data_off, 4);
	return check_vram_copy(s->cpu[BO_DATA] + data_off);
}

static int step_ttm_gtt(struct st *s)
{
	int r = step_ttm_move(s, AMDGPU_GEM_DOMAIN_GTT, ST_IB_MOVE_GTT, ST_MOVE_GTT_OFF,
			      &s->res->gtt_moved, &s->res->gtt_ns, &s->res->gtt_value);

	/* VRAM is no longer allowed: validation had to move the buffer. */
	if (!r && s->res->gtt_moved < ST_BO_BYTES)
		r = RT_CS_MISMATCH;
	return r;
}

static int step_ttm_vram(struct st *s)
{
	/* Whether the buffer moves back now depends on the submission's move
	 * budget (GTT stays allowed); the copy is checked either way. */
	return step_ttm_move(s, AMDGPU_GEM_DOMAIN_VRAM, ST_IB_MOVE_VRAM, ST_MOVE_VRAM_OFF,
			     &s->res->vram_moved, &s->res->vram_ns, &s->res->vram_back_value);
}

/* Undo everything that was done, in reverse; the first error counts. */
static int teardown(struct st *s)
{
	int first = 0, r;

#define KEEP(x) do { r = (x); if (r && !first) first = r; } while (0)
	for (int i = 0; i < BO_COUNT; ++i)
		if (s->map_type[i])
			KEEP(rt_lx_munmap(s->c, s->map_type[i]));
	for (int i = 0; i < BO_COUNT; ++i)
		if (s->mapped_va[i])
			KEEP(gem_va(s, i, AMDGPU_VA_OP_UNMAP));
	for (int i = 0; i < BO_COUNT; ++i) {
		if (!s->bo[i])
			continue;
		memset(&s->arg.gem_close, 0, sizeof(s->arg.gem_close));
		s->arg.gem_close.handle = s->bo[i];
		KEEP((int)st_ioctl(s, DRM_IOCTL_GEM_CLOSE, &s->arg.gem_close));
	}
	for (int i = 0; i < 2; ++i) {
		if (!s->syncobj[i])
			continue;
		memset(&s->arg.syncobj_destroy, 0, sizeof(s->arg.syncobj_destroy));
		s->arg.syncobj_destroy.handle = s->syncobj[i];
		KEEP((int)st_ioctl(s, DRM_IOCTL_SYNCOBJ_DESTROY, &s->arg.syncobj_destroy));
	}
	if (s->ctx) {
		memset(&s->arg.ctx, 0, sizeof(s->arg.ctx));
		s->arg.ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
		s->arg.ctx.in.ctx_id = s->ctx_id;
		KEEP((int)st_ioctl(s, DRM_IOCTL_AMDGPU_CTX, &s->arg.ctx));
	}
	if (s->fd >= 0)
		KEEP(rt_lx_close(s->c, s->fd));
#undef KEEP
	rt_lx_client_destroy(s->c);
	return first;
}

/* Whether every submission has completed: AMDGPU_WAIT_CS with a zero
 * deadline polls. */
static bool idle(struct st *s)
{
	for (unsigned int i = 0; i < s->nsubs; ++i) {
		memset(&s->arg.wait_cs, 0, sizeof(s->arg.wait_cs));
		s->arg.wait_cs.in.handle = s->subs[i].seq;
		s->arg.wait_cs.in.ip_type = s->subs[i].ip_type;
		s->arg.wait_cs.in.ctx_id = s->ctx_id;
		if (st_ioctl(s, DRM_IOCTL_AMDGPU_WAIT_CS, &s->arg.wait_cs) ||
		    s->arg.wait_cs.out.status)
			return false;
	}
	return true;
}

/* Test processes whose submissions never completed. Their teardown would
 * wait for those jobs as a dying Linux process does (with GPU recovery
 * off, until the GPU signals them), so they are kept here instead and
 * torn down once idle (rt_cs_selftest_reap). */
#define ST_PARKED_MAX 4
static struct st *parked[ST_PARKED_MAX];
static pthread_mutex_t parked_lock = PTHREAD_MUTEX_INITIALIZER;

static bool park(struct st *s)
{
	bool kept = false;

	pthread_mutex_lock(&parked_lock);
	for (unsigned int i = 0; i < ST_PARKED_MAX && !kept; ++i) {
		if (!parked[i]) {
			parked[i] = s;
			kept = true;
		}
	}
	pthread_mutex_unlock(&parked_lock);
	return kept;
}

unsigned int rt_cs_selftest_reap(void)
{
	unsigned int left = 0;

	pthread_mutex_lock(&parked_lock);
	for (unsigned int i = 0; i < ST_PARKED_MAX; ++i) {
		struct st *s = parked[i];

		if (!s)
			continue;
		if (!idle(s)) {
			left++;
			continue;
		}
		(void)teardown(s);
		kvfree(s);
		parked[i] = NULL;
	}
	pthread_mutex_unlock(&parked_lock);
	return left;
}

unsigned int rt_cs_selftest_parked(void)
{
	unsigned int n = 0;

	pthread_mutex_lock(&parked_lock);
	for (unsigned int i = 0; i < ST_PARKED_MAX; ++i)
		n += !!parked[i];
	pthread_mutex_unlock(&parked_lock);
	return n;
}

static void record(struct st *s, enum rt_cs_step step, int r, int *first)
{
	s->res->status[step] = r;
	if (!r)
		s->res->passed |= 1u << step;
	else if (r != RT_CS_SKIPPED && !*first) {
		*first = r;
		s->res->failed_step = step;
	}
}

int rt_cs_selftest_run(struct pci_dev *pdev, struct rt_cs_selftest_result *out)
{
	struct drm_device *ddev = pdev ? pci_get_drvdata(pdev) : NULL;
	struct st *s;
	int first = 0, r;
	enum rt_cs_step step;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->version = RT_CS_SELFTEST_VERSION;
	out->steps = RT_CS_STEP_COUNT;
	out->failed_step = RT_CS_STEP_COUNT;
	for (int i = 0; i < RT_CS_STEP_COUNT; ++i)
		out->status[i] = RT_CS_NOT_RUN;
	if (!ddev || !ddev->render)
		return -ENODEV;
	s = kvzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->res = out;
	s->adev = drm_to_adev(ddev);
	s->fd = -1;
	init_completion(&s->done);

	r = rt_lx_client_create(pdev, 0, "cs-selftest", &s->c);
	if (!r) {
		s->fd = rt_lx_open(s->c, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC);
		r = s->fd < 0 ? s->fd : 0;
	}
	record(s, RT_CS_STEP_OPEN, r, &first);
	for (step = RT_CS_STEP_VERSION; !first && step < RT_CS_STEP_TEARDOWN; ++step) {
		switch (step) {
		case RT_CS_STEP_VERSION: r = step_version(s); break;
		case RT_CS_STEP_DEV_INFO: r = step_dev_info(s); break;
		case RT_CS_STEP_HW_IP: r = step_hw_ip(s); break;
		case RT_CS_STEP_CTX: r = step_ctx(s); break;
		case RT_CS_STEP_SYNCOBJ: r = step_syncobj(s); break;
		case RT_CS_STEP_GEM_CREATE: r = step_gem_create(s); break;
		case RT_CS_STEP_GEM_VA: r = step_gem_va(s); break;
		case RT_CS_STEP_GEM_MMAP: r = step_gem_mmap(s); break;
		case RT_CS_STEP_COMPUTE_CS:
			r = compute_engine(s) ? step_compute_cs(s) : RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_COMPUTE_WAIT_CS:
			r = compute_engine(s) ? step_compute_wait(s) : RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_COMPUTE_SYNCOBJ:
			r = compute_engine(s) ? wait_syncobj(s, 0) : RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_COMPUTE_RESULT:
			r = compute_engine(s) ? step_compute_result(s) : RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_SDMA_FILL:
			r = sdma_engine(s) ? step_sdma_fill(s) : RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_SDMA_COPY:
			r = out->status[RT_CS_STEP_SDMA_FILL] ? RT_CS_SKIPPED : step_sdma_copy(s);
			break;
		case RT_CS_STEP_SDMA_WAIT:
			r = out->status[RT_CS_STEP_SDMA_FILL] ? RT_CS_SKIPPED : step_sdma_wait(s);
			break;
		case RT_CS_STEP_SDMA_RESULT:
			/* The copy carries the compute value: both engines. */
			r = out->status[RT_CS_STEP_SDMA_FILL] ? RT_CS_SKIPPED :
				!out->status[RT_CS_STEP_COMPUTE_RESULT] ? step_sdma_result(s) :
				RT_CS_SKIPPED;
			break;
		case RT_CS_STEP_TTM_GTT:
			/* The VRAM buffer's known contents are what moves. */
			r = out->status[RT_CS_STEP_SDMA_RESULT] ? RT_CS_SKIPPED : step_ttm_gtt(s);
			break;
		case RT_CS_STEP_TTM_VRAM:
			r = out->status[RT_CS_STEP_TTM_GTT] ? RT_CS_SKIPPED : step_ttm_vram(s);
			break;
		default:
			r = -EINVAL;
			break;
		}
		record(s, step, r, &first);
	}
	if (s->c && s->fd >= 0 && !idle(s)) {
		/* Something is still running on the GPU: keep the process.
		 * Its later teardown must not write the caller's result. */
		s->parked_res = *out;
		s->res = &s->parked_res;
		if (park(s)) {
			out->status[RT_CS_STEP_TEARDOWN] = RT_CS_PARKED;
			if (!first) {
				first = RT_CS_PARKED;
				out->failed_step = RT_CS_STEP_TEARDOWN;
			}
			return first;
		}
		s->res = out;	/* no room: tear down and wait */
	}
	r = s->c ? teardown(s) : 0;
	record(s, RT_CS_STEP_TEARDOWN, r, &first);
	kvfree(s);
	return first;
}
