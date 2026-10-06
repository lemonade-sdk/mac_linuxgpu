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
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <drm/drm_ioctl.h>
#include <rt/kfd_session.h>
#include <rt/lx_files.h>
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

/* ---- the Linux-file transport (lx_files) as libhsakmt uses /dev/kfd ---- */

/* The fixture maps no GEM objects through a render file. */
int rt_lx_gem_map(struct drm_device *ddev, struct pci_dev *pdev, struct vm_area_struct *vma,
		  uint64_t length, void **pinned, uint32_t *backing, uint32_t *cache,
		  int (*add)(void *, uint32_t, uint64_t, uint64_t), void *arg)
{
	(void)ddev; (void)pdev; (void)vma; (void)length; (void)pinned; (void)backing;
	(void)cache; (void)add; (void)arg;
	return -ENODEV;
}
void rt_lx_gem_unpin(void *pinned) { (void)pinned; }

/* ioctl(2) as libmlg_drm frames it: this test's memory is the client's. */
static long lx_ioctl(struct rt_lx_client *c, uint32_t dev, int fd, uint32_t cmd, void *arg)
{
	static struct mlg_lx_span spans[MLG_LX_DESCRIBE_MAX];
	static uint8_t frame[16384], rep[16384];
	uint64_t timeout_va = 0, out = 0;
	size_t rep_bytes = 0;
	int64_t result = 0;
	uint32_t n = 0;
	long len;

	if (mlg_lx_describe(dev, cmd, (uint64_t)(uintptr_t)arg, spans, MLG_LX_DESCRIBE_MAX, &n,
			    &timeout_va))
		return -ENOTTY;
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va, 0, frame,
			    sizeof(frame), &out);
	assert(len > 0);
	assert(!rt_lx_ioctl(c, fd, cmd, frame, (size_t)len, rep, sizeof(rep), &rep_bytes, &result));
	assert(!mlg_lx_apply_reply(frame, (size_t)len, rep, rep_bytes, NULL));
	return (long)result;
}

/* hsaKmtOpenKFD and hsakmt_fmm_init_process_apertures through the
 * transport: a client process opening /dev/kfd and the render node itself,
 * the apertures array written through its pointer, ACQUIRE_VM finding the
 * render descriptor in the same process's table. */
static void lx_kfd_client(void)
{
	static struct pci_dev pdev;
	struct rt_lx_client *c = NULL;
	struct kfd_ioctl_get_version_args version = {0};
	struct kfd_ioctl_get_process_apertures_new_args apn = {0};
	struct kfd_process_device_apertures nodes[4];
	struct kfd_ioctl_acquire_vm_args acquire = {0};
	const unsigned int opens = render_opens, acquires = vm_acquires;
	int kfd_fd, drm_fd;

	pci_set_drvdata(&pdev, &adev->ddev);
	assert(!rt_lx_client_create(&pdev, 5150, "lx-hsakmt", &c) && rt_lx_client_pid(c) == 5150);
	kfd_fd = rt_lx_open(c, MLG_LX_DEV_KFD, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC);
	drm_fd = rt_lx_open(c, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC);
	assert(kfd_fd >= 0 && drm_fd >= 0 && render_opens == opens + 1);
	assert(!lx_ioctl(c, MLG_LX_DEV_KFD, kfd_fd, AMDKFD_IOC_GET_VERSION, &version));
	assert(version.major_version == KFD_IOCTL_MAJOR_VERSION);
	/* The count, then the array. */
	assert(!lx_ioctl(c, MLG_LX_DEV_KFD, kfd_fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &apn));
	assert(apn.num_of_nodes == 1);
	memset(nodes, 0, sizeof(nodes));
	apn.kfd_process_device_apertures_ptr = (uint64_t)(uintptr_t)nodes;
	assert(!lx_ioctl(c, MLG_LX_DEV_KFD, kfd_fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &apn));
	assert(nodes[0].gpu_id == TEST_GPU_ID && nodes[0].gpuvm_limit == (1ULL << 47) - 1);
	acquire.gpu_id = TEST_GPU_ID;
	acquire.drm_fd = drm_fd;
	assert(!lx_ioctl(c, MLG_LX_DEV_KFD, kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &acquire));
	assert(vm_acquires == acquires + 1);
	/* Not a KFD request: refused before the device. */
	assert(lx_ioctl(c, MLG_LX_DEV_KFD, kfd_fd, AMDKFD_IOC_SVM, &apn) == -ENOTTY);
	/* Exit: KFD's notifier release, then both files close. */
	rt_lx_client_destroy(c);
}

/* ---- a client that dies with live queues (process death) ----
 * The client's process is gone; the dext closes its session with its
 * queues still mapped by MES and its memory still allocated, as Linux
 * tears down a process that dies with live queues. */
static struct rt_kfd_session *dying_client(const char *comm, struct queue_set *q,
					   struct rt_kfd_bo **vram)
{
	struct rt_kfd_session *s;
	uint64_t size;

	assert(!rt_kfd_session_open(adev, &compute_ctx, 0, comm, &s));
	size = rt_kfd_session_window_size(s);
	assert(!rt_kfd_session_set_window(s, size * 4, 0));
	make_queue(s, q, 64);
	assert(!rt_kfd_queue_kick(s, q->q, 1));
	assert(!rt_kfd_bo_alloc(s, 4 << 20, 0, RT_KFD_VRAM, RT_KFD_PLACE_PRIVATE, vram));
	return s;
}

static void process_death(void)
{
	const unsigned int resets_before = gpu_reset_requests;
	struct queue_set q;
	struct rt_kfd_bo *vram;
	struct rt_kfd_session *s;
	unsigned int removes, failed, hang_resets, resumes, frees;
	int error = 0;

	/* 1. A wave that never preempts: MES's REMOVE_QUEUE times out inside
	 * DESTROY_QUEUE (KFD asks for a GPU reset, which recovery-off turns
	 * into a log line). The session recovers the queue as upstream
	 * recovers a hung user queue (MES resets it, then removes it after
	 * the reset), and the close completes. */
	s = dying_client("killed-hung", &q, &vram);
	fixture_mes_hang(q.info.doorbell_index, true);
	removes = mes_removes;
	failed = mes_failed_removes;
	hang_resets = mes_hang_resets;
	resumes = mes_resumes;
	frees = kgd_frees;
	assert(!rt_kfd_session_close(s));
	assert(mes_failed_removes == failed + 1 && gpu_reset_requests == resets_before + 1);
	assert(mes_hang_resets == hang_resets + 1 && mes_resumes == resumes + 1);
	assert(mes_removes == removes + 1);
	assert(kgd_frees > frees && kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);

	/* 2. A slow MES acknowledgement within its API timeout is no failure. */
	s = dying_client("killed-slow", &q, &vram);
	mes_remove_delay_us = 200 * 1000;
	removes = mes_removes;
	failed = mes_failed_removes;
	assert(!rt_kfd_session_close(s));
	mes_remove_delay_us = 0;
	assert(mes_removes == removes + 1 && mes_failed_removes == failed);
	assert(kgd_frees == kgd_allocs);

	/* 3. MES never answers: nothing confirms the queue is off the GPU, so
	 * the close keeps the session, its queue and every buffer, and names
	 * the step. Once MES answers again, closing again recovers the queue
	 * and completes. */
	s = dying_client("killed-mes-dead", &q, &vram);
	frees = kgd_frees;
	mes_dead = true;
	assert(rt_kfd_session_close(s) == -EBUSY);
	assert(rt_kfd_session_uncertain(s));
	assert(rt_kfd_session_failure(s, &error) == RT_KFD_STEP_DESTROY_QUEUE &&
	       error == -ETIMEDOUT);
	assert(kgd_frees == frees);
	{
		struct rt_kfd_bo *more = NULL;

		assert(rt_kfd_bo_alloc(s, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_PRIVATE, &more) == -EBUSY);
		assert(rt_kfd_queue_kick(s, q.q, 2) == -EBUSY);
	}
	assert(rt_kfd_session_settle(s, 0) == -EBUSY);
	mes_dead = false;
	removes = mes_removes;
	assert(!rt_kfd_session_close(s));
	assert(mes_removes == removes + 1);
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);

	/* 4. A queue the owner destroys while MES does not answer is retried
	 * by a settle, which brings the session back into service. */
	s = dying_client("hung-destroy", &q, &vram);
	mes_dead = true;
	assert(rt_kfd_queue_destroy(s, q.q) == -ETIMEDOUT);
	assert(rt_kfd_session_uncertain(s));
	mes_dead = false;
	assert(!rt_kfd_session_settle(s, 0) && !rt_kfd_session_uncertain(s));
	assert(rt_kfd_queue_kick(s, q.q, 3) == -ENODEV);	/* off the GPU */
	assert(!rt_kfd_queue_destroy(s, q.q));
	assert(rt_kfd_session_queue_count(s) == 0);
	assert(!rt_kfd_bo_free(s, q.ring) && !rt_kfd_bo_free(s, q.meta));
	assert(!rt_kfd_session_close(s));

	/* 5. An SDMA copy that outlives its timeout keeps the staging and the
	 * buffer; when the engine catches up the session works again, and a
	 * close waits for it once more before keeping anything. */
	s = dying_client("copy-timeout", &q, &vram);
	{
		unsigned char word[64];

		sdma_hold = true;
		assert(rt_kfd_bo_read(s, vram, 0, word, sizeof(word)) == -ETIMEDOUT);
		assert(rt_kfd_session_uncertain(s));
		assert(rt_kfd_session_failure(s, &error) == RT_KFD_STEP_COPY);
		assert(rt_kfd_bo_read(s, vram, 0, word, sizeof(word)) == -EBUSY);
		assert(rt_kfd_session_settle(s, 0) == -EBUSY);
		assert(rt_kfd_session_close(s) == -EBUSY);	/* bounded */
		fixture_sdma_release();
		assert(!rt_kfd_session_settle(s, 0));
		assert(!rt_kfd_bo_read(s, vram, 0, word, sizeof(word)));
	}
	assert(!rt_kfd_session_close(s));
	assert(kgd_frees == kgd_allocs);
	/* 6. The device leaves the bus mid-session: MES answers nothing, a copy
	 * is still queued and a queue still runs. Nothing on the bus can reach
	 * the session's memory, so the close drops the queue and the copy and
	 * frees everything at once. */
	s = dying_client("unplugged", &q, &vram);
	{
		unsigned char word[64];

		sdma_hold = true;
		assert(rt_kfd_bo_read(s, vram, 0, word, sizeof(word)) == -ETIMEDOUT);
		assert(rt_kfd_session_uncertain(s));
	}
	mes_dead = true;
	fixture_remove_device(true);
	assert(!rt_kfd_session_close(s));
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);
	fixture_remove_device(false);
	mes_dead = false;
	fixture_sdma_finish(false);	/* the removed device never ran it */
	puts("KFD process death: hung queue recovered through MES reset, slow MES "
	     "acknowledgement, MES that never answers (kept, then recovered), "
	     "destroy retried by settle, SDMA copy that outlived its timeout, device "
	     "removed mid-session");
}


/* ---- signal events: interrupt-driven waits ---- */

struct waiter {
	struct rt_kfd_session *s;
	uint32_t id;
	uint32_t timeout_ms;
	int ret;
	uint32_t result;
	uint64_t elapsed_ms;
	pthread_t thread;
};

static uint64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void *waiter_main(void *arg)
{
	struct waiter *w = arg;
	struct rt_kfd_wait *wait = NULL;
	const uint64_t start = now_ms();
	uint32_t result = KFD_IOC_WAIT_RESULT_FAIL;
	int ret = rt_kfd_wait_begin(w->s, &w->id, 1, 0, w->timeout_ms, &wait);

	if (!ret)
		ret = rt_kfd_wait_run(wait, &result);
	w->result = result;
	w->elapsed_ms = now_ms() - start;
	__atomic_store_n(&w->ret, ret, __ATOMIC_RELEASE);	/* 1 until it returns */
	return NULL;
}

static void start_waiter(struct waiter *w, struct rt_kfd_session *s, uint32_t id, uint32_t timeout_ms)
{
	memset(w, 0, sizeof(*w));
	w->s = s;
	w->id = id;
	w->timeout_ms = timeout_ms;
	w->ret = 1;
	assert(!pthread_create(&w->thread, NULL, waiter_main, w));
}

/* One AQL dispatch on @set whose completion signal (an amd_signal_t at
 * @signal_offset of @host, value 1) carries @event: the fixture's CP
 * decrements the value, writes the mailbox and raises the interrupt. */
static void dispatch_with_signal(struct rt_kfd_session *s, struct queue_set *set, uint64_t *next_id,
				 struct rt_kfd_bo *host, uint64_t signal_offset,
				 const struct rt_kfd_event *event)
{
	struct rt_kfd_bo_info ring, meta, hostinfo;
	uint64_t abi[8] = {1, 1, event ? event->mailbox_va : 0, event ? event->trigger : 0};
	uint8_t packet[64] = {0};
	uint64_t signal, id = (*next_id)++;
	uint32_t packets;

	assert(!rt_kfd_bo_info(s, set->ring, &ring) && !rt_kfd_bo_info(s, set->meta, &meta) &&
	       !rt_kfd_bo_info(s, host, &hostinfo));
	packets = (uint32_t)(ring.size / 64);
	signal = hostinfo.va + signal_offset;
	assert(!rt_kfd_bo_write(s, host, signal_offset, abi, sizeof(abi)));
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_RING_BASE, &ring.va, 8));
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_RING_SIZE, &packets, 4));
	packet[0] = FIXTURE_AQL_PACKET_DISPATCH;
	memcpy(packet + FIXTURE_AQL_COMPLETION, &signal, 8);
	assert(!rt_kfd_bo_write(s, set->ring, (id % packets) * 64, packet, sizeof(packet)));
	id++;
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_WRITE_ID, &id, 8));
	assert(!rt_kfd_queue_kick(s, set->q, id - 1));
}

static int64_t signal_value(struct rt_kfd_session *s, struct rt_kfd_bo *host, uint64_t offset)
{
	int64_t v = 0;

	assert(!rt_kfd_bo_read(s, host, offset + 8, &v, 8));
	return v;
}

static void events_and_waits(struct rt_kfd_session *s, struct queue_set *set, struct rt_kfd_bo *host)
{
	struct rt_kfd_event e0, e1, e2;
	struct waiter a, b, c;
	uint64_t next_id = 0, start;
	unsigned int interrupts;

	/* CREATE_EVENT: the first hands KFD the event page. */
	assert(!rt_kfd_event_create(s, &e0) && !rt_kfd_event_create(s, &e1));
	assert(rt_kfd_event_count(s) == 2);
	assert(e0.id != e1.id && e0.trigger == e0.id && e1.trigger == e1.id);
	assert(e0.mailbox_va != e1.mailbox_va && e0.mailbox_va >= 3ULL << 45);
	{
		/* KFD filled the page with UNSIGNALED_EVENT_SLOT. */
		uint64_t *slot = fixture_va_to_host(TEST_PASID, e1.mailbox_va, 8);
		assert(slot && *slot == UINT64_MAX);
	}
	/* Waits name live events only. */
	{
		struct rt_kfd_wait *w = NULL;
		uint32_t bogus = 4000;
		assert(rt_kfd_wait_begin(s, &bogus, 1, 0, 10, &w) == -ENOENT && !w);
	}
	fixture_cp_start();

	/* The interrupt wakes the waiter on its event and no other. */
	start_waiter(&a, s, e0.id, 1000);
	start_waiter(&b, s, e1.id, 300);
	usleep(20000);
	interrupts = fixture_cp_interrupts();
	start = now_ms();
	dispatch_with_signal(s, set, &next_id, host, 0, &e0);
	assert(!pthread_join(a.thread, NULL));
	assert(!a.ret && a.result == KFD_IOC_WAIT_RESULT_COMPLETE);
	assert(now_ms() - start < 200 && fixture_cp_interrupts() == interrupts + 1);
	assert(signal_value(s, host, 0) == 0);
	assert(!pthread_join(b.thread, NULL));
	assert(!b.ret && b.result == KFD_IOC_WAIT_RESULT_TIMEOUT && b.elapsed_ms >= 290);
	printf("events: interrupt woke its waiter in %llu ms; the other slept to its %u ms timeout\n",
	       (unsigned long long)a.elapsed_ms, b.timeout_ms);

	/* An interrupt that never arrives: the wait ends at its timeout (the
	 * runtime's backstop) and the signal is found complete then. */
	fixture_cp_drop_interrupts(true);
	start_waiter(&a, s, e0.id, 50);
	usleep(5000);
	dispatch_with_signal(s, set, &next_id, host, 64, &e0);
	assert(!pthread_join(a.thread, NULL));
	assert(!a.ret && a.result == KFD_IOC_WAIT_RESULT_TIMEOUT && a.elapsed_ms >= 45);
	assert(signal_value(s, host, 64) == 0 && fixture_cp_interrupts_dropped() == 1);
	fixture_cp_drop_interrupts(false);

	/* An interrupt before the wait: the auto-reset event stays signaled
	 * until a wait consumes it. */
	dispatch_with_signal(s, set, &next_id, host, 128, &e1);
	start = now_ms();
	while (signal_value(s, host, 128) != 0 && now_ms() - start < 1000)
		usleep(100);
	start_waiter(&a, s, e1.id, 1000);
	assert(!pthread_join(a.thread, NULL));
	assert(!a.ret && a.result == KFD_IOC_WAIT_RESULT_COMPLETE && a.elapsed_ms < 100);

	/* SET_EVENT (a host-side store) wakes a waiter. */
	start_waiter(&a, s, e1.id, 1000);
	usleep(20000);
	assert(!rt_kfd_event_set(s, e1.id));
	assert(!pthread_join(a.thread, NULL));
	assert(!a.ret && a.result == KFD_IOC_WAIT_RESULT_COMPLETE && a.elapsed_ms < 500);

	/* DESTROY_EVENT ends a wait on it. */
	assert(!rt_kfd_event_create(s, &e2));
	start_waiter(&c, s, e2.id, 1000);
	usleep(20000);
	assert(!rt_kfd_event_destroy(s, e2.id) && rt_kfd_event_count(s) == 2);
	assert(!pthread_join(c.thread, NULL));
	/* KFD reports a wait whose event went away as -EIO, result FAIL. */
	assert(c.ret == -EIO && c.result == KFD_IOC_WAIT_RESULT_FAIL && c.elapsed_ms < 500);
	assert(rt_kfd_event_destroy(s, e2.id) == -ENOENT);

	fixture_cp_stop();
}

/* ---- a GPU page fault of one client while another runs ----
 * The faulting client destroyed a completion signal (freed its memory)
 * while a dispatch naming it was still queued: the CP's signal write
 * faults. As on Linux, KFD's interrupt handler evicts that process's
 * queues and signals its memory event; the session reports the fault, its
 * queues never run again, and closing it is an ordinary close. The other
 * client's queue keeps running before, during and after. */
static void dispatch_to(struct rt_kfd_session *s, struct queue_set *set, uint64_t *next_id,
			uint64_t signal)
{
	struct rt_kfd_bo_info ring;
	uint8_t packet[64] = {0};
	uint64_t id = (*next_id)++;
	uint32_t packets;

	assert(!rt_kfd_bo_info(s, set->ring, &ring));
	packets = (uint32_t)(ring.size / 64);
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_RING_BASE, &ring.va, 8));
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_RING_SIZE, &packets, 4));
	packet[0] = FIXTURE_AQL_PACKET_DISPATCH;
	memcpy(packet + FIXTURE_AQL_COMPLETION, &signal, 8);
	assert(!rt_kfd_bo_write(s, set->ring, (id % packets) * 64, packet, sizeof(packet)));
	id++;
	assert(!rt_kfd_bo_write(s, set->meta, FIXTURE_AQL_WRITE_ID, &id, 8));
	assert(!rt_kfd_queue_kick(s, set->q, id - 1));
}

static bool bystander_completes(struct rt_kfd_session *s, struct queue_set *set, uint64_t *next_id,
				struct rt_kfd_bo *host)
{
	const uint64_t start = now_ms();

	dispatch_with_signal(s, set, next_id, host, 0, NULL);
	while (signal_value(s, host, 0) != 0)
		if (now_ms() - start > 2000)
			return false;
		else
			usleep(100);
	return true;
}

static void vm_fault_isolation(void)
{
	struct queue_set qa, qb;
	struct rt_kfd_bo *vram_a, *vram_b, *host_b, *doomed;
	struct rt_kfd_bo_info doomed_info;
	struct rt_kfd_session *a, *b;
	struct rt_kfd_fault fault;
	uint64_t next_a = 0, next_b = 0, start;
	unsigned int removes, faults;

	fixture_cp_start();
	a = dying_client("faulting", &qa, &vram_a);
	b = dying_client("bystander", &qb, &vram_b);
	assert(!rt_kfd_bo_alloc(b, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &host_b));
	assert(bystander_completes(b, &qb, &next_b, host_b));
	assert(rt_kfd_session_fault(a, &fault) == 0 && rt_kfd_session_fault(b, &fault) == 0);

	/* Destroy before completion: the signal's memory goes, then the
	 * dispatch that names it runs. */
	assert(!rt_kfd_bo_alloc(a, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &doomed));
	assert(!rt_kfd_bo_info(a, doomed, &doomed_info));
	assert(!rt_kfd_bo_free(a, doomed));
	removes = mes_removes;
	faults = fixture_vm_faults();
	dispatch_to(a, &qa, &next_a, doomed_info.va);
	start = now_ms();
	while (rt_kfd_session_fault(a, &fault) == 0) {
		assert(now_ms() - start < 2000);
		usleep(100);
	}
	assert(fixture_vm_faults() == faults + 1);
	/* The faulting 4 KiB GPU page, not the CPU page. */
	assert(fault.va == ((doomed_info.va + 8) & ~(uint64_t)(AMDGPU_GPU_PAGE_SIZE - 1)));
	assert(fault.not_present);
	/* KFD took the faulting process's queue off MES, and only that one. */
	assert(mes_removes == removes + 1);
	assert(rt_kfd_queue_kick(a, qa.q, 5) == -EFAULT);
	assert(rt_kfd_session_fault(a, NULL) == 1);	/* sticky */
	assert(hqd_dumps > 0 && !fixture_cp_stuck());	/* HQDs read, nothing left on them */
	assert(!rt_kfd_session_uncertain(a));

	/* The other client neither sees the fault nor stops. */
	assert(rt_kfd_session_fault(b, &fault) == 0 && !rt_kfd_session_uncertain(b));
	assert(bystander_completes(b, &qb, &next_b, host_b));

	/* The faulting client leaves: an ordinary close, nothing kept. */
	assert(!rt_kfd_session_close(a));
	assert(mes_removes == removes + 1);	/* its queue was already off MES */
	assert(bystander_completes(b, &qb, &next_b, host_b));
	assert(!rt_kfd_session_close(b));
	fixture_cp_stop();
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);
	puts("KFD GPU page fault: the faulting process's queues evicted and its memory event "
	     "signaled, the fault reported and sticky, its close ordinary; the other client ran "
	     "throughout");
}

/* ---- a faulted queue left on the command processor ----
 * What wedged the R9700 twice: KFD evicted the faulting process's queue
 * and MES confirmed removing it, yet the CP, stalled on the faulting
 * access, kept running it on its HQD, and nothing else on that pipe
 * completed again. The session reads the HQDs when it sees the fault and
 * again at close: a queue still there is reset through MES's hung-queue
 * reset, and one that survives that makes it request a GPU reset and keep
 * its memory until the HQDs are clear. */
static struct rt_kfd_session *faulting_client(const char *comm, struct queue_set *q,
					      uint64_t *signal_va)
{
	struct rt_kfd_bo *vram, *doomed;
	struct rt_kfd_bo_info info;
	struct rt_kfd_session *s = dying_client(comm, q, &vram);
	uint64_t next = 0;

	assert(!rt_kfd_bo_alloc(s, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &doomed));
	assert(!rt_kfd_bo_info(s, doomed, &info));
	assert(!rt_kfd_bo_free(s, doomed));
	*signal_va = info.va;
	dispatch_to(s, q, &next, info.va);
	return s;
}

static void wait_for_fault_interrupt(unsigned int before)
{
	const uint64_t start = now_ms();

	while (fixture_vm_faults() == before) {
		assert(now_ms() - start < 2000);
		usleep(100);
	}
}

static void vm_fault_stuck_on_cp(void)
{
	struct queue_set qa, qb, qc;
	struct rt_kfd_bo *vram_b, *host_b;
	struct rt_kfd_session *a, *b;
	struct rt_kfd_fault fault;
	uint64_t next_b = 0, va;
	unsigned int resets, hang_resets, faults, dumps;
	int error = 0;

	fixture_cp_start();
	b = dying_client("bystander", &qb, &vram_b);
	assert(!rt_kfd_bo_alloc(b, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &host_b));

	/* 1. Stuck through MES's removal, cleared by its hung-queue reset when
	 * the fault is first seen. */
	fixture_cp_fault_sticks(true, false);
	resets = gpu_reset_requests;
	hang_resets = mes_hang_resets;
	faults = fixture_vm_faults();
	dumps = hqd_dumps;
	a = faulting_client("stuck-mes-reset", &qa, &va);
	wait_for_fault_interrupt(faults);
	assert(fixture_cp_stuck() == 1);	/* MES said removed; the HQD still runs it */
	assert(rt_kfd_session_fault(a, &fault) == 1);
	assert(hqd_dumps > dumps && mes_hang_resets == hang_resets + 1);
	assert(fixture_cp_stuck() == 0 && gpu_reset_requests == resets);
	assert(!rt_kfd_session_uncertain(a));
	assert(bystander_completes(b, &qb, &next_b, host_b));
	assert(!rt_kfd_session_close(a));

	/* 2. Stuck through MES's reset too: a GPU reset is requested, the
	 * session keeps its memory (uncertain, close refused) until a settle
	 * finds the HQDs clear. */
	fixture_cp_fault_sticks(true, true);
	fixture_gpu_reset_defer(true);
	faults = fixture_vm_faults();
	a = faulting_client("stuck-gpu-reset", &qa, &va);
	wait_for_fault_interrupt(faults);
	resets = gpu_reset_requests;
	assert(rt_kfd_session_fault(a, &fault) == 1);
	assert(gpu_reset_requests == resets + 1);	/* requested once */
	assert(rt_kfd_session_failure(a, &error) == RT_KFD_STEP_CP_STUCK);
	assert(rt_kfd_session_uncertain(a) && fixture_cp_stuck() == 1);
	/* While the reset has not happened: the close keeps everything, a
	 * settle too, and no second reset is asked for. */
	assert(rt_kfd_session_close(a) == -EBUSY);
	assert(rt_kfd_session_settle(a, 0) == -EBUSY && gpu_reset_requests == resets + 1);
	assert(bystander_completes(b, &qb, &next_b, host_b));
	fixture_gpu_reset_finish();
	fixture_gpu_reset_defer(false);
	assert(!rt_kfd_session_settle(a, 0) && !rt_kfd_session_uncertain(a));
	assert(!rt_kfd_session_close(a));
	assert(bystander_completes(b, &qb, &next_b, host_b));

	/* 3. The fault never polled before the program leaves: the close
	 * itself finds the queue on its HQD and resets it first. */
	fixture_cp_fault_sticks(true, false);
	faults = fixture_vm_faults();
	hang_resets = mes_hang_resets;
	a = faulting_client("stuck-at-close", &qc, &va);
	wait_for_fault_interrupt(faults);
	assert(fixture_cp_stuck() == 1);
	assert(!rt_kfd_session_close(a));
	assert(mes_hang_resets == hang_resets + 1 && fixture_cp_stuck() == 0);
	assert(bystander_completes(b, &qb, &next_b, host_b));

	fixture_cp_fault_sticks(false, false);
	assert(!rt_kfd_session_close(b));
	fixture_cp_stop();
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);
	puts("KFD faulted queue left on the CP: found on its HQD when the fault is seen and at "
	     "close, cleared by MES's hung-queue reset, or by a GPU reset with the session kept "
	     "until the HQDs are clear; the other client ran throughout");
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
		/* A VRAM allocation the VRAM manager cannot place without eviction
		 * is refused before KFD sees it; one that fits is not. */
		struct ttm_resource_manager *man = &adev->mman.vram_mgr.manager;
		struct rt_kfd_bo *tight = NULL;

		spin_lock_init(&adev->mman.bdev.lru_lock);
		man->size = TEST_VRAM_BYTES;
		man->usage = TEST_VRAM_BYTES - (160ULL << 20);
		assert(rt_kfd_bo_alloc(s, 64ULL << 20, 0, RT_KFD_VRAM, RT_KFD_PLACE_PRIVATE, &tight) == -ENOMEM);
		assert(!tight && kgd_allocs == 2);
		assert(!rt_kfd_bo_alloc(s, 16ULL << 20, 0, RT_KFD_VRAM, RT_KFD_PLACE_PRIVATE, &tight) && tight);
		assert(kgd_allocs == 3);
		assert(!rt_kfd_bo_free(s, tight));
		man->size = 0;
		man->usage = 0;
	}
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

	lx_kfd_client();

	/* ---- DESTROY_QUEUE ---- */
	assert(!rt_kfd_queue_destroy(s, q0.q));
	assert(mes_removes == 2 && rt_kfd_session_queue_count(s) == 1);
	assert(!rt_kfd_bo_free(s, q0.ring) && !rt_kfd_bo_free(s, q0.meta));
	make_queue(s, &q2, 64);
	assert(mes_adds == 4 && q2.info.doorbell_index == mes_doorbells[3]);

	/* ---- signal events and interrupt-driven waits ---- */
	{
		struct waiter late;
		uint32_t id;

		events_and_waits(s, &q2, host);
		/* A wait running when the session closes: close destroys the
		 * events, the wait ends (FAIL), and close goes on once it has. */
		{
			struct rt_kfd_event e;
			assert(!rt_kfd_event_create(s, &e));
			id = e.id;
		}
		start_waiter(&late, s, id, 1000);
		usleep(20000);
		assert(__atomic_load_n(&late.ret, __ATOMIC_ACQUIRE) == 1);	/* still asleep */
		assert(!rt_kfd_session_close(s));
		assert(!pthread_join(late.thread, NULL));
		assert(late.ret == -EIO && late.result == KFD_IOC_WAIT_RESULT_FAIL);
		assert(late.elapsed_ms < 900);
	}
	assert(mes_removes == 4);

	process_death();
	assert(kgd_frees == kgd_allocs && kgd_unmaps == kgd_maps);
	vm_fault_isolation();
	vm_fault_stuck_on_cp();
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
