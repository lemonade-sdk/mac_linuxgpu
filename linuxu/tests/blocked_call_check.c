/* A Linux-file call that does not return must hold nothing but its own
 * worker (blocked_call_check.h), on the CS fixture's device.
 *
 * On build 241 a request that waited on a GPU ring that had stopped ran on
 * the driver's incoming-call thread, the one thread every client's calls
 * and the driver's own Stop arrive on: every client froze, and the driver
 * could not take its Stop after the GPU was powered off. Here a client's
 * AMDGPU_CS blocks the way one does on a stopped ring (its context has
 * amdgpu_sched_jobs submissions the compute engine never runs, so the next
 * waits in amdgpu_ctx_wait_prev_fence), and while it does:
 *   - every call the test makes as the incoming-call thread would (start
 *     an async call, a synchronous call, a Stop) returns at once;
 *   - the blocked client's own synchronous request runs, and its sleeping
 *     request on the synchronous path is refused without running;
 *   - another client opens the render node, allocates and submits, and its
 *     work completes;
 *   - the blocked client's Stop hands its exit to a thread of its own and
 *     returns; the exit finishes once its work completes.
 * blocked_call_park and blocked_call_after_removal put a blocked client
 * through a surprise removal (removal_check.c): removal ends the blocked
 * call, and the client's exit completes. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/amdgpu_drm.h>
#include <drm/drm.h>
#include <rt/lx_abi.h>
#include <rt/lx_files.h>
#include <rt/recovery.h>

#include "amdgpu.h"
#include "amdgpu_reset.h"
#include "nvd.h"
#include "cs_fixture.h"
#include "blocked_call_check.h"

extern int usleep(unsigned int usec);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

/* What "at once" means for a call on the incoming-call thread: it queues
 * work or runs a request that cannot sleep. Generous for a loaded host. */
#define CALL_MS_MAX	200
#define BO_BYTES	(64u << 10)

struct bc_call {
	struct completion done;
	int64_t result;
	uint8_t rep[256];
	size_t bytes;
	int kept;
	uint64_t token;
	void *frame;
	size_t frame_bytes;
	void *arg;
};

struct bc_client {
	struct rt_lx_client *c;
	int fd;
	uint32_t ctx, bo;
	uint64_t va;
	uint32_t *ib;
	uint32_t ib_dw;
	struct drm_amdgpu_bo_list_entry list[1];
	struct drm_amdgpu_bo_list_in list_in;
	struct drm_amdgpu_cs_chunk_ib chunk_ib;
	struct drm_amdgpu_cs_chunk chunks[2];
	uint64_t chunk_ptrs[2];
	union drm_amdgpu_cs cs;
	struct bc_call blocked;		/* the submission that waits */
};

static void bc_done(void *ctx, uint64_t token, int64_t result, const void *rbuf, size_t bytes)
{
	struct bc_call *call = ctx;

	CHECK(token == call->token || !call->token);
	call->result = result;
	call->bytes = bytes;
	if (!rbuf && bytes)
		call->kept = 1;
	else if (bytes) {
		CHECK(bytes <= sizeof(call->rep));
		memcpy(call->rep, rbuf, bytes);
	}
	complete(&call->done);
}

static ktime_t call_start;
static void on_call_thread(void)
{
	call_start = ktime_get();
}
static void returned_at_once(void)
{
	CHECK(ktime_ms_delta(ktime_get(), call_start) < CALL_MS_MAX);
}

static void *build(uint32_t cmd, void *arg, size_t *bytes, uint64_t *out_bytes)
{
	struct mlg_lx_span *spans = calloc(MLG_LX_DESCRIBE_MAX, sizeof(*spans));
	uint64_t timeout_va = 0;
	uint32_t n = 0;
	void *frame;
	long len;

	CHECK(spans);
	CHECK(!mlg_lx_describe(MLG_LX_DEV_RENDER, cmd, (uint64_t)(uintptr_t)arg, spans,
			       MLG_LX_DESCRIBE_MAX, &n, &timeout_va));
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va, ktime_get_ns(),
			    NULL, 0, out_bytes);
	CHECK(len > 0);
	frame = malloc((size_t)len);
	CHECK(frame);
	CHECK(mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va, ktime_get_ns(),
			    frame, (size_t)len, out_bytes) == len);
	free(spans);
	*bytes = (size_t)len;
	return frame;
}

/* The synchronous LX_IOCTL, as the incoming-call thread runs it. */
static long call_sync(struct bc_client *k, uint32_t cmd, void *arg)
{
	uint64_t out_bytes = 0;
	size_t bytes = 0, rep_bytes = 0;
	void *frame = build(cmd, arg, &bytes, &out_bytes);
	void *rep = malloc(mlg_lx_reply_bytes(out_bytes));
	int64_t result = 0;
	int r;

	CHECK(rep);
	on_call_thread();
	r = rt_lx_ioctl_nosleep(k->c, k->fd, cmd, frame, bytes, rep, mlg_lx_reply_bytes(out_bytes),
				&rep_bytes, &result);
	returned_at_once();
	if (!r)
		CHECK(!mlg_lx_apply_reply(frame, bytes, rep, rep_bytes, NULL));
	free(rep);
	free(frame);
	return r ? r : (long)result;
}

/* LX_IOCTL_ASYNC: started on the incoming-call thread, which returns. */
static void call_start_async(struct bc_client *k, uint32_t cmd, void *arg, struct bc_call *call)
{
	uint64_t out_bytes = 0;

	memset(call, 0, sizeof(*call));
	init_completion(&call->done);
	call->arg = arg;
	call->frame = build(cmd, arg, &call->frame_bytes, &out_bytes);
	on_call_thread();
	CHECK(!rt_lx_ioctl_async(k->c, k->fd, cmd, call->frame, call->frame_bytes, bc_done, call,
				 &call->token));
	returned_at_once();
}

/* Whether the call completed within @ms; its reply applied, as the client
 * library applies it. */
static int call_finished(struct bc_client *k, struct bc_call *call, unsigned int ms)
{
	if (!wait_for_completion_timeout(&call->done, msecs_to_jiffies(ms)))
		return 0;
	if (call->kept) {
		static uint8_t rep[1 << 16];
		size_t rep_bytes = 0;
		int64_t result = 0;

		CHECK(!rt_lx_result(k->c, call->token, rep, sizeof(rep), &rep_bytes, &result));
		CHECK(!mlg_lx_apply_reply(call->frame, call->frame_bytes, rep, rep_bytes, NULL));
	} else if (call->bytes) {
		CHECK(!mlg_lx_apply_reply(call->frame, call->frame_bytes, call->rep, call->bytes,
					  NULL));
	}
	free(call->frame);
	call->frame = NULL;
	return 1;
}

static long call_async(struct bc_client *k, uint32_t cmd, void *arg)
{
	struct bc_call call;

	call_start_async(k, cmd, arg, &call);
	CHECK(call_finished(k, &call, 5000));
	return (long)call.result;
}

/* LX_CALL_ASYNC. */
static int64_t op(struct bc_client *k, const uint64_t *in, uint32_t nin, uint64_t *words)
{
	struct bc_call call;

	memset(&call, 0, sizeof(call));
	init_completion(&call.done);
	on_call_thread();
	CHECK(!rt_lx_op_async(k->c, in, nin, bc_done, &call, &call.token));
	returned_at_once();
	CHECK(wait_for_completion_timeout(&call.done, msecs_to_jiffies(5000)));
	if (words && !call.result) {
		CHECK(call.bytes == MLG_LX_OP_MMAP_WORDS * 8);
		memcpy(words, call.rep, call.bytes);
	}
	return call.result;
}

/* A client with the render node open, a context and an IB buffer of
 * compute NOPs mapped at a GPU address. */
static void client_open(struct pci_dev *pdev, const char *name, struct bc_client *k)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	uint64_t open_in[] = { MLG_LX_OP_OPEN, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC };
	struct drm_amdgpu_info_device dev = { 0 };
	struct drm_amdgpu_info info = { .return_pointer = (uint64_t)(uintptr_t)&dev,
					.return_size = sizeof(dev), .query = AMDGPU_INFO_DEV_INFO };
	union drm_amdgpu_ctx ctx = { .in = { .op = AMDGPU_CTX_OP_ALLOC_CTX,
					     .priority = AMDGPU_CTX_PRIORITY_NORMAL } };
	union drm_amdgpu_gem_create create = { .in = { .bo_size = BO_BYTES, .alignment = PAGE_SIZE,
		.domains = AMDGPU_GEM_DOMAIN_GTT, .domain_flags = AMDGPU_GEM_CREATE_CPU_GTT_USWC } };
	struct drm_amdgpu_gem_va va = { 0 };
	union drm_amdgpu_gem_mmap gmap = { 0 };
	uint64_t words[MLG_LX_OP_MMAP_WORDS], contiguous = 0;
	const uint32_t nop = adev->gfx.compute_ring[0].funcs->nop;

	memset(k, 0, sizeof(*k));
	CHECK(!rt_lx_client_create(pdev, 0, name, &k->c));
	k->fd = (int)op(k, open_in, MLG_LX_OP_OPEN_ARGS, NULL);
	CHECK(k->fd >= 0);
	CHECK(call_async(k, DRM_IOCTL_AMDGPU_INFO, &info) == 0);
	CHECK(call_async(k, DRM_IOCTL_AMDGPU_CTX, &ctx) == 0);
	k->ctx = ctx.out.alloc.ctx_id;
	CHECK(call_async(k, DRM_IOCTL_AMDGPU_GEM_CREATE, &create) == 0);
	k->bo = create.out.handle;
	k->va = ALIGN(dev.virtual_address_offset, 2ull << 20) + (2ull << 20);
	va = (struct drm_amdgpu_gem_va){ .handle = k->bo, .operation = AMDGPU_VA_OP_MAP,
		.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE |
			 AMDGPU_VM_PAGE_EXECUTABLE,
		.va_address = k->va, .map_size = BO_BYTES };
	CHECK(call_async(k, DRM_IOCTL_AMDGPU_GEM_VA, &va) == 0);
	gmap.in.handle = k->bo;
	CHECK(call_async(k, DRM_IOCTL_AMDGPU_GEM_MMAP, &gmap) == 0);
	uint64_t mmap_in[] = { MLG_LX_OP_MMAP, (uint64_t)k->fd, gmap.out.addr_ptr, BO_BYTES,
			       MLG_LX_PROT_READ | MLG_LX_PROT_WRITE, MLG_LX_MAP_SHARED };
	CHECK(op(k, mmap_in, MLG_LX_OP_MMAP_ARGS, words) == 0);
	k->ib = rt_lx_map_cpu(k->c, words[0], 0, &contiguous);
	CHECK(k->ib && contiguous >= BO_BYTES);
	/* Eight NOPs: whatever runs them (once the queue does) does nothing. */
	k->ib_dw = 8;
	for (uint32_t i = 0; i < k->ib_dw; ++i)
		k->ib[i] = nop;
}

/* One AMDGPU_CS of the NOP IB on compute. */
static void cs_args(struct bc_client *k)
{
	k->list[0] = (struct drm_amdgpu_bo_list_entry){ .bo_handle = k->bo };
	k->list_in = (struct drm_amdgpu_bo_list_in){ .operation = ~0u, .list_handle = ~0u,
		.bo_number = 1, .bo_info_size = sizeof(k->list[0]),
		.bo_info_ptr = (uint64_t)(uintptr_t)k->list };
	k->chunk_ib = (struct drm_amdgpu_cs_chunk_ib){ .ip_type = AMDGPU_HW_IP_COMPUTE,
		.va_start = k->va, .ib_bytes = k->ib_dw * 4 };
	k->chunks[0] = (struct drm_amdgpu_cs_chunk){ AMDGPU_CHUNK_ID_BO_HANDLES,
		sizeof(k->list_in) / 4, (uint64_t)(uintptr_t)&k->list_in };
	k->chunks[1] = (struct drm_amdgpu_cs_chunk){ AMDGPU_CHUNK_ID_IB, sizeof(k->chunk_ib) / 4,
		(uint64_t)(uintptr_t)&k->chunk_ib };
	k->chunk_ptrs[0] = (uint64_t)(uintptr_t)&k->chunks[0];
	k->chunk_ptrs[1] = (uint64_t)(uintptr_t)&k->chunks[1];
	k->cs = (union drm_amdgpu_cs){ .in = { .ctx_id = k->ctx, .num_chunks = 2,
					       .chunks = (uint64_t)(uintptr_t)k->chunk_ptrs } };
}

/* With the compute engine held: amdgpu_sched_jobs submissions are taken,
 * and the next waits for the first, which never runs. */
static void block_a_submission(struct bc_client *k)
{
	extern int amdgpu_sched_jobs;

	for (int i = 0; i < amdgpu_sched_jobs; ++i) {
		cs_args(k);
		CHECK(call_async(k, DRM_IOCTL_AMDGPU_CS, &k->cs) == 0);
	}
	cs_args(k);
	call_start_async(k, DRM_IOCTL_AMDGPU_CS, &k->cs, &k->blocked);
	CHECK(!call_finished(k, &k->blocked, 300));
}

struct stop_done {
	volatile int finished;
};
static void stop_finished(void *arg)
{
	__atomic_store_n(&((struct stop_done *)arg)->finished, 1, __ATOMIC_RELEASE);
}

void blocked_call_check(struct pci_dev *pdev)
{
	struct bc_client a, b;
	struct stop_done stopped = { 0 };
	ktime_t start = ktime_get();

	client_open(pdev, "blocked-client", &a);
	cs_fixture_hold_compute(1);
	block_a_submission(&a);

	/* The blocked client's process still takes a request that cannot
	 * sleep, and refuses one that can on the synchronous path. */
	struct drm_syncobj_create sc = { 0 };
	CHECK(call_sync(&a, DRM_IOCTL_SYNCOBJ_CREATE, &sc) == 0 && sc.handle);
	cs_args(&a);
	CHECK(call_sync(&a, DRM_IOCTL_AMDGPU_CS, &a.cs) == -EDEADLK);
	union drm_amdgpu_ctx query = { .in = { .op = AMDGPU_CTX_OP_QUERY_STATE2, .ctx_id = a.ctx } };
	CHECK(call_sync(&a, DRM_IOCTL_AMDGPU_CTX, &query) == -EDEADLK);

	/* Another client works meanwhile: open, allocate, submit to the SDMA
	 * engine (the compute one is held) and see it complete. */
	client_open(pdev, "other-client", &b);
	struct drm_syncobj_create done = { 0 };
	CHECK(call_sync(&b, DRM_IOCTL_SYNCOBJ_CREATE, &done) == 0);
	cs_args(&b);
	/* SDMA NOP (the fixture's SDMA NOP is a zero dword). */
	uint32_t *sdma_ib = b.ib + 256;
	memset(sdma_ib, 0, 8 * 4);
	b.chunk_ib = (struct drm_amdgpu_cs_chunk_ib){ .ip_type = AMDGPU_HW_IP_DMA,
		.va_start = b.va + 1024, .ib_bytes = 8 * 4 };
	struct drm_amdgpu_cs_chunk_sem out = { .handle = done.handle };
	struct drm_amdgpu_cs_chunk chunks[3] = { b.chunks[0], b.chunks[1],
		{ AMDGPU_CHUNK_ID_SYNCOBJ_OUT, sizeof(out) / 4, (uint64_t)(uintptr_t)&out } };
	uint64_t ptrs[3] = { (uint64_t)(uintptr_t)&chunks[0], (uint64_t)(uintptr_t)&chunks[1],
			     (uint64_t)(uintptr_t)&chunks[2] };
	b.cs.in.num_chunks = 3;
	b.cs.in.chunks = (uint64_t)(uintptr_t)ptrs;
	CHECK(call_async(&b, DRM_IOCTL_AMDGPU_CS, &b.cs) == 0);
	uint32_t handles[1] = { done.handle };
	struct drm_syncobj_wait wait = { .handles = (uint64_t)(uintptr_t)handles,
		.count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
		.timeout_nsec = (int64_t)(ktime_get_ns() + 5000000000ull) };
	CHECK(call_async(&b, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0);
	/* Polling it is a request that cannot sleep. */
	wait.timeout_nsec = 0;
	CHECK(call_sync(&b, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0);
	CHECK(!completion_done(&a.blocked.done));

	/* The blocked client stops (its process exits on a thread of its
	 * own); the Stop returns at once. */
	on_call_thread();
	CHECK(!rt_lx_client_retire(a.c, stop_finished, &stopped));
	returned_at_once();
	/* Its blocked call ends with the exit (the kill interrupts the
	 * wait); its work on the held engine keeps the exit going until
	 * the engine runs. Client b is unaffected. */
	CHECK(wait_for_completion_timeout(&a.blocked.done, msecs_to_jiffies(5000)));
	free(a.blocked.frame);
	CHECK(call_sync(&b, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0);
	cs_fixture_hold_compute(0);
	for (int i = 0; i < 1000 && !__atomic_load_n(&stopped.finished, __ATOMIC_ACQUIRE); ++i)
		usleep(5000);
	CHECK(__atomic_load_n(&stopped.finished, __ATOMIC_ACQUIRE));
	rt_lx_retire_drain();
	CHECK(rt_lx_retiring() == 0);
	rt_lx_client_destroy(b.c);
	printf("PASS blocked call: a submission waiting on a held queue keeps only its worker; "
	       "calls return at once, the blocked client's non-sleeping request runs and its "
	       "sleeping one is refused synchronously, another client opens, allocates and "
	       "completes work, the blocked client's Stop returns at once and its exit "
	       "finishes when its work does (%lld ms)\n",
	       (long long)ktime_ms_delta(ktime_get(), start));
}

/* ---- through a surprise removal (removal_check.c) ---- */

static struct bc_client parked;
static struct stop_done parked_stopped;

void blocked_call_park(struct pci_dev *pdev)
{
	client_open(pdev, "removal-blocked", &parked);
	/* The caller holds the compute engine. */
	block_a_submission(&parked);
}

void blocked_call_after_removal(void)
{
	/* Removal completed the held work, so the blocked submission went on
	 * (and failed against a device that is gone, or ran to nothing). */
	CHECK(wait_for_completion_timeout(&parked.blocked.done, msecs_to_jiffies(2000)));
	free(parked.blocked.frame);
	on_call_thread();
	CHECK(!rt_lx_client_retire(parked.c, stop_finished, &parked_stopped));
	returned_at_once();
	for (int i = 0; i < 400 && !__atomic_load_n(&parked_stopped.finished, __ATOMIC_ACQUIRE); ++i)
		usleep(5000);
	CHECK(__atomic_load_n(&parked_stopped.finished, __ATOMIC_ACQUIRE));
	rt_lx_retire_drain();
	CHECK(rt_lx_retiring() == 0);
	printf("PASS blocked call through removal: the removal ends a submission blocked on a "
	       "held queue (result %lld) and the client's exit completes\n",
	       (long long)parked.blocked.result);
}

/* ---- a hung queue under GPU recovery ----
 *
 * Linux's recovery, unmodified: a compute job that never completes times
 * out (drm_sched), amdgpu_job_timedout resets its queue (the fixture's
 * fx_ring_reset, as MES resets a kernel queue), the guilty job's fence
 * gets -ETIME, the jobs queued behind it from other contexts run, the
 * guilty context reports the reset and refuses further submissions, and a
 * new context works. Nothing waits forever, nothing escalates. */

#define RESET_VALUE	0xb0b0cafeu
#define RESET_DATA_OFF	4096u

/* Client @k's IB: one WRITE_DATA of @value to its buffer at @off. */
static void write_ib(struct bc_client *k, uint32_t value, uint32_t off)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	const uint32_t nop = adev->gfx.compute_ring[0].funcs->nop;
	const uint64_t dst = k->va + off;
	uint32_t n = 0;

	k->ib[n++] = PACKET3(PACKET3_WRITE_DATA, 3);
	k->ib[n++] = (5u << 8) | (1u << 20);	/* memory, write confirm */
	k->ib[n++] = lower_32_bits(dst);
	k->ib[n++] = upper_32_bits(dst);
	k->ib[n++] = value;
	while (n % 8)
		k->ib[n++] = nop;
	k->ib_dw = n;
}

void queue_reset_check(struct pci_dev *pdev)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	struct amdgpu_ring *ring = &adev->gfx.compute_ring[0];
	const long saved_timeout = ring->sched.timeout;
	const int resets = atomic_read(&adev->gpu_reset_counter);
	struct cs_fixture_stats before, after;
	struct rt_recovery_state was, now;
	volatile uint32_t *written;
	struct bc_client a, b;
	ktime_t start = ktime_get();
	long r;

	rt_recovery_state(&was);
	client_open(pdev, "guilty-client", &a);
	client_open(pdev, "innocent-client", &b);
	write_ib(&a, 0x11111111u, RESET_DATA_OFF);
	write_ib(&b, RESET_VALUE, RESET_DATA_OFF);
	written = (volatile uint32_t *)((uint8_t *)b.ib + RESET_DATA_OFF);
	*written = 0;

	/* A's job hangs (the queue does not run it); B's waits behind it. */
	ring->sched.timeout = msecs_to_jiffies(300);
	cs_fixture_stats(&before);
	cs_fixture_hold_compute(1);
	cs_args(&a);
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CS, &a.cs) == 0);
	cs_args(&b);
	CHECK(call_async(&b, DRM_IOCTL_AMDGPU_CS, &b.cs) == 0);

	/* The timeout fires, the queue resets, B's job runs. */
	for (int i = 0; i < 1000 && *written != RESET_VALUE; ++i)
		usleep(5000);
	cs_fixture_stats(&after);
	CHECK(after.queue_resets == before.queue_resets + 1);
	CHECK(*written == RESET_VALUE);
	CHECK(atomic_read(&adev->gpu_reset_counter) == resets + 1);
	CHECK(!amdgpu_in_reset(adev));
	/* The reset generation advanced; nothing wedged. */
	rt_recovery_state(&now);
	CHECK(now.queue_resets == was.queue_resets + 1 && now.generation == was.generation + 1);
	CHECK(!(now.flags & RT_RECOVERY_WEDGED) && now.last_result == 0);

	/* A's context was guilty: it reports the reset and refuses work
	 * (its last job's error, as Linux returns it). */
	union drm_amdgpu_ctx query = { .in = { .op = AMDGPU_CTX_OP_QUERY_STATE2, .ctx_id = a.ctx } };
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CTX, &query) == 0);
	CHECK(query.out.state.flags & AMDGPU_CTX_QUERY2_FLAGS_RESET);
	cs_args(&a);
	r = call_async(&a, DRM_IOCTL_AMDGPU_CS, &a.cs);
	CHECK(r == -ETIME || r == -ECANCELED);

	/* A new context of the same process works. */
	union drm_amdgpu_ctx ctx = { .in = { .op = AMDGPU_CTX_OP_ALLOC_CTX,
					     .priority = AMDGPU_CTX_PRIORITY_NORMAL } };
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CTX, &ctx) == 0);
	a.ctx = ctx.out.alloc.ctx_id;
	volatile uint32_t *mine = (volatile uint32_t *)((uint8_t *)a.ib + RESET_DATA_OFF);
	*mine = 0;
	write_ib(&a, 0xa0a0beefu, RESET_DATA_OFF);
	cs_args(&a);
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CS, &a.cs) == 0);
	for (int i = 0; i < 400 && *mine != 0xa0a0beefu; ++i)
		usleep(5000);
	CHECK(*mine == 0xa0a0beefu);

	/* The innocent client goes on as before. */
	*written = 0;
	cs_args(&b);
	CHECK(call_async(&b, DRM_IOCTL_AMDGPU_CS, &b.cs) == 0);
	for (int i = 0; i < 400 && *written != RESET_VALUE; ++i)
		usleep(5000);
	CHECK(*written == RESET_VALUE);

	ring->sched.timeout = saved_timeout;
	cs_fixture_hold_compute(0);
	rt_lx_client_destroy(a.c);
	rt_lx_client_destroy(b.c);
	printf("PASS queue reset: a hung compute job times out, its queue resets (no device reset), "
	       "the job behind it from another process runs, the guilty context reports the reset "
	       "and refuses work, a new context works (%lld ms)\n",
	       (long long)ktime_ms_delta(ktime_get(), start));
}

/* ---- a hang no queue reset ends ----
 *
 * The queue reset fails (MES does not answer): upstream would reset the
 * device, which is not available over Thunderbolt yet, so the device
 * wedges instead (rt/recovery.h). The blocked work completes with an
 * error, every new request fails with -ENODEV, the state says wedged, and
 * nothing tries a device reset (the fixture has no ASIC reset: one would
 * crash this test). Runs in a process of its own: the device stays
 * wedged. */
void wedge_check(struct pci_dev *pdev)
{
	struct amdgpu_device *adev = cs_fixture_adev();
	struct amdgpu_ring *ring = &adev->gfx.compute_ring[0];
	struct rt_recovery_state st;
	struct bc_client a, b;
	struct bc_call blocked;
	ktime_t start = ktime_get();

	client_open(pdev, "hung-client", &a);
	client_open(pdev, "bystander", &b);
	write_ib(&a, 0x11111111u, RESET_DATA_OFF);
	ring->sched.timeout = msecs_to_jiffies(300);
	cs_fixture_fail_queue_reset(1);
	cs_fixture_hold_compute(1);
	cs_args(&a);
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CS, &a.cs) == 0);

	/* B waits on that queue too: a CS of its own behind A's, then a
	 * wait on its fence with no deadline. Both end with the wedge. */
	struct drm_syncobj_create done = { 0 };
	CHECK(call_sync(&b, DRM_IOCTL_SYNCOBJ_CREATE, &done) == 0);
	cs_args(&b);
	struct drm_amdgpu_cs_chunk_sem out = { .handle = done.handle };
	struct drm_amdgpu_cs_chunk chunks[3] = { b.chunks[0], b.chunks[1],
		{ AMDGPU_CHUNK_ID_SYNCOBJ_OUT, sizeof(out) / 4, (uint64_t)(uintptr_t)&out } };
	uint64_t ptrs[3] = { (uint64_t)(uintptr_t)&chunks[0], (uint64_t)(uintptr_t)&chunks[1],
			     (uint64_t)(uintptr_t)&chunks[2] };
	b.cs.in.num_chunks = 3;
	b.cs.in.chunks = (uint64_t)(uintptr_t)ptrs;
	CHECK(call_async(&b, DRM_IOCTL_AMDGPU_CS, &b.cs) == 0);
	uint32_t handles[1] = { done.handle };
	struct drm_syncobj_wait wait = { .handles = (uint64_t)(uintptr_t)handles,
		.count_handles = 1, .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
		.timeout_nsec = INT64_MAX };
	call_start_async(&b, DRM_IOCTL_SYNCOBJ_WAIT, &wait, &blocked);

	/* The timeout fires, the queue reset fails, the device wedges. */
	for (int i = 0; i < 1000; ++i) {
		rt_recovery_state(&st);
		if (st.flags & RT_RECOVERY_WEDGED)
			break;
		usleep(5000);
	}
	rt_recovery_state(&st);
	CHECK(st.flags & RT_RECOVERY_WEDGED);
	CHECK(st.queue_resets == 0 && st.generation == 1);
	CHECK(!amdgpu_in_reset(adev) && adev->no_hw_access);
	/* The wait with no deadline ends (its fence completed, cancelled). */
	CHECK(call_finished(&b, &blocked, 2000));
	/* New requests fail as for a device that is gone. */
	struct drm_syncobj_create more = { 0 };
	CHECK(call_sync(&b, DRM_IOCTL_SYNCOBJ_CREATE, &more) == -ENODEV);
	cs_args(&a);
	CHECK(call_async(&a, DRM_IOCTL_AMDGPU_CS, &a.cs) == -ENODEV);

	rt_recovery_end();
	printf("PASS wedge: a hang the queue reset cannot end wedges the device instead of a "
	       "device reset; blocked work and a wait with no deadline end, new requests fail "
	       "with -ENODEV, the state says wedged (%lld ms)\n",
	       (long long)ktime_ms_delta(ktime_get(), start));
}
