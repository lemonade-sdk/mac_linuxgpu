/* KFD compute sessions (rt/kfd_session.h) over upstream KFD.
 *
 * Links the unmodified KFD character device (kfd_chardev.c: kfd_open,
 * kfd_ioctl, kfd_mmap), process and process-queue managers, the device
 * queue manager with its GFX 12 asic ops and MQD manager, KFD's queue
 * buffer validation (kfd_queue.c), doorbells, apertures, events, debug and
 * SMI state, on the linuxu process substrate. A session opens /dev/kfd and
 * a render node in its process's descriptor table and runs libhsakmt's open
 * sequence (GET_VERSION, GET_PROCESS_APERTURES_NEW, ACQUIRE_VM,
 * SET_MEMORY_POLICY, RUNTIME_ENABLE) through kfd_ioctl with arguments in
 * per-call user VMAs; then ALLOC + MAP of GTT and VRAM, two CREATE_QUEUEs
 * (two MES ADD_QUEUEs with distinct doorbells, no legacy HQD map), kicks
 * through the doorbell kfd_doorbell_mmap() io_remap_pfn_range()d, copies,
 * a second client process, DESTROY_QUEUE and close, and finally the KFD
 * exit path, with every kmalloc allocation released. The amdgpu side is
 * kfd_session_fixture.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <drm/drm_ioctl.h>
#include <rt/kfd_session.h>
#include "amdgpu.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_topology.h"
#include "kfd_session_fixture.h"

/* The context-save header of queue @set, read back through KFD's BO. */
struct queue_set {
	struct rt_kfd_bo *ring, *meta;
	struct rt_kfd_queue *q;
	struct rt_kfd_queue_info info;
};

/* The runtime's per-queue buffers: an AQL ring of @packets and the
 * amd_queue_t page whose read/write dispatch ids KFD gets as pointers. */
#define TEST_READ_ID_OFFSET	128
#define TEST_WRITE_ID_OFFSET	56
static void make_queue(struct rt_kfd_session *s, struct queue_set *set, uint32_t packets)
{
	struct rt_kfd_bo_info ring, meta;
	struct rt_kfd_queue_desc desc = {0};

	assert(!rt_kfd_bo_alloc(s, packets * 64, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &set->ring));
	assert(!rt_kfd_bo_alloc(s, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &set->meta));
	assert(!rt_kfd_bo_info(s, set->ring, &ring) && !rt_kfd_bo_info(s, set->meta, &meta));
	desc.ring = set->ring;
	desc.ring_bytes = packets * 64;
	desc.read_pointer = meta.va + TEST_READ_ID_OFFSET;
	desc.write_pointer = meta.va + TEST_WRITE_ID_OFFSET;
	int r = rt_kfd_queue_create(s, &desc, &set->q);
	if (r)
		fprintf(stderr, "kfd_session test: CREATE_QUEUE failed: %d\n", r);
	assert(!r);
	assert(!rt_kfd_queue_info(s, set->q, &set->info));
}

struct range_count { unsigned int runs; uint64_t bytes; };
static int count_range(void *arg, void *cpu, uint64_t bytes)
{
	struct range_count *c = arg;
	assert(cpu && bytes && !(bytes % PAGE_SIZE));
	c->runs++;
	c->bytes += bytes;
	return 0;
}

static void check_ctx_header(const struct queue_set *set,
			     const struct rt_kfd_queue_limits *limits)
{
	uint32_t *header = fixture_va_to_host(fixture_pasid_of(set->info.ctx_save_va),
					      set->info.ctx_save_va, 24);

	assert(header);
	assert(header[4] == limits->xcc_count * limits->ctx_save_bytes);	/* DebugOffset */
	assert(header[5] == limits->debug_bytes * limits->xcc_count);	/* DebugSize */
}

int main(void)
{
	struct rt_kfd_session *s, *second;
	struct rt_kfd_apertures ap;
	struct rt_kfd_queue_limits limits;
	struct queue_set q0, q1, q2, other;
	struct rt_kfd_bo *code, *host;
	struct rt_kfd_bo_info code_info, host_info;
	uint64_t window_base, window_size;
	size_t live_at_start;
	unsigned char pattern[3 * 16384 + 100], back[sizeof(pattern)];

	fixture_device_init();
	/* kfd_init's character device and process half. */
	fixture_kfd_init();
	/* Every allocation a session and its KFD processes make must be gone
	 * once they are closed and released. */
	live_at_start = kmemcheck_live_bytes();
	fixture_kfd_wq_init();
	assert(!rt_kfd_session_supported(adev));

	/* ---- open: libhsakmt's sequence through kfd_ioctl ---- */
	assert(!rt_kfd_session_open(adev, &compute_ctx, 4242, "hrx-client", &s));
	assert(render_opens == 1 && vm_acquires == 1);
	assert(rt_kfd_session_pid(s) == 4242);
	assert(!rt_kfd_session_apertures(s, &ap));
	assert(ap.gpu_id == TEST_GPU_ID);
	assert(ap.version_major == KFD_IOCTL_MAJOR_VERSION &&
	       ap.version_minor == KFD_IOCTL_MINOR_VERSION);
	/* kfd_init_apertures_v9, which KFD uses for GC 9.0.1 up to 12.1 */
	assert(ap.gpuvm_base == AMDGPU_VA_RESERVED_BOTTOM && ap.gpuvm_limit == (1ULL << 47) - 1);
	assert(ap.lds_base == 1ULL << 48 && ap.scratch_base == 2ULL << 48);
	assert(mes_shader_debugger_sets == 1);	/* RUNTIME_ENABLE on MES */
	assert(!rt_kfd_session_queue_limits(s, &limits));
	assert(limits.slots == 127 && limits.eop_bytes == topo.node_props.eop_buffer_size);
	assert(limits.ctx_save_bytes == topo.node_props.cwsr_size &&
	       limits.ctl_stack_bytes == topo.node_props.ctl_stack_size);
	assert(limits.doorbell_slice_bytes == kfd_doorbell_process_slice(&kfd));

	/* ---- the host window: CPU VA == GPU VA for shared buffers ---- */
	window_size = rt_kfd_session_window_size(s);
	assert(window_size >= (4ULL << 30) && !(window_size & (window_size - 1)));
	assert(!rt_kfd_session_window(s, &window_base, &window_size) && !window_base);
	assert(rt_kfd_bo_alloc(s, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &host) == -EINVAL);
	assert(rt_kfd_session_set_window(s, window_size + 4096, 0) == -EINVAL);
	assert(rt_kfd_session_set_window(s, 3ULL << 45, 0) == -EINVAL);	/* the private range */
	assert(rt_kfd_session_set_window(s, window_size * 4, window_size * 2) == -EINVAL);
	assert(rt_kfd_session_set_window(s, window_size * 4, 3ULL << 20) == -EINVAL);
	assert(!rt_kfd_session_set_window(s, window_size * 4, 0));
	assert(rt_kfd_session_set_window(s, window_size * 8, 0) == -EBUSY);
	assert(!rt_kfd_session_set_window(s, window_size * 4, window_size));
	assert(!rt_kfd_session_window(s, &window_base, &window_size) &&
	       window_base == window_size * 4);

	/* ---- memory: ALLOC_MEMORY_OF_GPU + MAP_MEMORY_TO_GPU ---- */
	assert(!rt_kfd_bo_alloc(s, sizeof(pattern), 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &host));
	assert(!rt_kfd_bo_alloc(s, 3 << 20, 0, RT_KFD_VRAM, RT_KFD_PLACE_PRIVATE, &code));
	assert(!rt_kfd_bo_info(s, host, &host_info) && !rt_kfd_bo_info(s, code, &code_info));
	assert(host_info.va >= window_base && host_info.va + host_info.size <= window_base + window_size);
	assert(host_info.domain == RT_KFD_GTT && host_info.size == ALIGN(sizeof(pattern), PAGE_SIZE));
	assert(code_info.domain == RT_KFD_VRAM && code_info.va >= 3ULL << 45 &&
	       !(code_info.va & ((2ULL << 20) - 1)));
	assert(GET_GPU_ID(host_info.handle) == TEST_GPU_ID);
	assert(kgd_allocs == 2 && kgd_maps == 2);
	{
		struct range_count c = {0};
		assert(!rt_kfd_bo_cpu_ranges(s, host, count_range, &c));
		assert(c.bytes == host_info.size && c.runs >= 1);
		assert(rt_kfd_bo_cpu_ranges(s, code, count_range, &c) == -EINVAL);
	}
	for (size_t i = 0; i < sizeof(pattern); ++i)
		pattern[i] = (unsigned char)(i * 131 + 7);
	/* GTT through its pages, VRAM through SDMA and the staging. */
	assert(!rt_kfd_bo_write(s, host, 0, pattern, sizeof(pattern)));
	assert(!rt_kfd_bo_copy(s, host, 0, code, 4096, sizeof(pattern)));
	assert(sdma_copies);
	memset(back, 0, sizeof(back));
	assert(!rt_kfd_bo_read(s, code, 4096, back, sizeof(back)));
	assert(!memcmp(back, pattern, sizeof(pattern)));
	assert(!rt_kfd_bo_write(s, code, 0, pattern, 100));
	assert(!rt_kfd_bo_copy(s, code, 0, host, 1000, 100));
	memset(back, 0, sizeof(back));
	assert(!rt_kfd_bo_read(s, host, 1000, back, 100) && !memcmp(back, pattern, 100));
	assert(rt_kfd_bo_read(s, host, host_info.size - 4, back, 8) == -ERANGE);

	/* ---- queues: two CREATE_QUEUEs become two MES ADD_QUEUEs ---- */
	make_queue(s, &q0, 64);
	make_queue(s, &q1, 256);
	assert(rt_kfd_session_queue_count(s) == 2);
	assert(mes_adds == 2 && mes_removes == 0);
	assert(mes_doorbells[0] != mes_doorbells[1]);
	assert(q0.info.doorbell_index == mes_doorbells[0] &&
	       q1.info.doorbell_index == mes_doorbells[1]);
	assert(mes_pasid[0] == TEST_PASID && mes_pasid[1] == TEST_PASID);
	assert(!cp_dispatches);	/* no packet was published */
	assert(gart_maps == 2);	/* each write pointer page reached MES through the GART */
	assert(q0.info.eop_va && q0.info.ctx_save_va && q0.info.eop_va != q1.info.eop_va);
	assert(q0.info.queue_id != q1.info.queue_id);
	check_ctx_header(&q0, &limits);
	/* Kicks write the doorbell KFD gave MES (64-bit doorbells). */
	assert(!rt_kfd_queue_kick(s, q0.q, 5));
	assert(!rt_kfd_queue_kick(s, q1.q, 9));
	assert(doorbell_bar[mes_doorbells[0] / 2] == 5 && doorbell_bar[mes_doorbells[1] / 2] == 9);
	/* The queue's buffers cannot go while it exists. */
	assert(rt_kfd_bo_free(s, q0.ring) == -EBUSY && rt_kfd_bo_free(s, q0.meta) == -EBUSY);

	/* A second client is a second KFD process: its own pid, VM, queues and
	 * doorbell slice. */
	assert(!rt_kfd_session_open(adev, &compute_ctx, 0, "second-client", &second));
	assert(rt_kfd_session_pid(second) > 0 && rt_kfd_session_pid(second) != 4242);
	assert(render_opens == 2 && vm_acquires == 2);
	/* A client keeping its GART-sized window: 512 MiB at its base. */
	assert(!rt_kfd_session_set_window(second, window_base, 512ULL << 20));
	{
		uint64_t b, z;
		assert(!rt_kfd_session_window(second, &b, &z) && b == window_base &&
		       z == 512ULL << 20);
	}
	make_queue(second, &other, 64);
	assert(mes_adds == 3 && other.info.doorbell_index == mes_doorbells[2]);
	assert(other.info.doorbell_index / (limits.doorbell_slice_bytes / 4) !=
	       q0.info.doorbell_index / (limits.doorbell_slice_bytes / 4));
	assert(!rt_kfd_queue_kick(second, other.q, 3));
	assert(doorbell_bar[mes_doorbells[2] / 2] == 3 && mes_pasid[2] == TEST_PASID + 1);
	/* Session close destroys the queue and frees the memory it left. */
	assert(!rt_kfd_session_close(second));
	assert(mes_removes == 1);

	/* ---- DESTROY_QUEUE ---- */
	assert(!rt_kfd_queue_destroy(s, q0.q));
	assert(mes_removes == 2 && rt_kfd_session_queue_count(s) == 1);
	assert(!rt_kfd_bo_free(s, q0.ring) && !rt_kfd_bo_free(s, q0.meta));
	make_queue(s, &q2, 64);
	assert(mes_adds == 4 && q2.info.doorbell_index == mes_doorbells[3]);

	/* ---- close: queues, memory, then the process exits ---- */
	assert(!rt_kfd_session_close(s));
	assert(mes_removes == 4);
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);
	/* kfd_exit order: the release work frees the KFD process, which drops
	 * the render file ACQUIRE_VM kept. */
	fixture_kfd_release_processes();
	assert(render_releases == render_opens);
	if (kmemcheck_live_bytes() != live_at_start)
		fprintf(stderr, "kfd_session test: %zu kmalloc bytes live, %zu before the sessions\n",
			kmemcheck_live_bytes(), live_at_start);
	assert(kmemcheck_live_bytes() == live_at_start);
	fixture_kfd_exit();
	fixture_device_fini();
	assert(!compute_ctx.bos);
	(void)fixture_report_bos();
	if (kernel_allocs)
		fprintf(stderr, "kfd_session test: %u kernel allocations left\n", kernel_allocs);
	assert(live_bos == 0 && kernel_allocs == 0);
	puts("KFD compute session over upstream KFD: open sequence, GTT/VRAM alloc+map, "
	     "two MES queues with distinct doorbells and no legacy HQD, kicks, copies, "
	     "destroy and close passed");
	return 0;
}
