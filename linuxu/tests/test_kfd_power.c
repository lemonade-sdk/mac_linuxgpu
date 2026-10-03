/* Compute power transitions (rt/power.h) through upstream amdkfd's PM entry
 * points: kgd2kfd_suspend(kfd, true) and kgd2kfd_resume(kfd, true) from the
 * unmodified kfd_device.c, over the KFD session fixture
 * (kfd_session_fixture.c) with a live KFD process, a MES queue and the
 * fixture's command processor executing its AQL packets.
 *
 * Checked: a suspend unmaps every user queue through MES (REMOVE_QUEUE) and
 * stops the queue manager, holds KFD locked against new processes, and
 * leaves work published meanwhile unexecuted; a resume restores the
 * process's buffers in place (a new eviction fence), maps the same queue
 * back (ADD_QUEUE, same doorbell) and the paused work runs; a session
 * closed while suspended issues no MES operation and the resume that
 * follows balances upstream's suspend count, so KFD takes new processes
 * again; a REMOVE_QUEUE that MES fails is reported (-EIO) through the reset
 * mark upstream's kfd_hws_hang() leaves, even though the queue reads as
 * inactive afterwards. Every allocation is released at the end. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <rt/kfd_session.h>
#include <rt/power.h>
#include "amdgpu.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_topology.h"
#include "kfd_session_fixture.h"

extern int usleep(unsigned int usec);

/* The runtime's buffers for one AQL queue: the ring and the amd_queue_t
 * page (read/write dispatch ids at the offsets KFD gets as pointers), and a
 * completion signal in the same page. */
#define SIGNAL_OFFSET	1024
struct queue_set {
	struct rt_kfd_bo *ring, *meta;
	struct rt_kfd_queue *q;
	struct rt_kfd_queue_info info;
	uint64_t ring_va, meta_va;
	uint32_t packets, pasid;
};

static void make_queue(struct rt_kfd_session *s, struct queue_set *set, uint32_t packets)
{
	struct rt_kfd_bo_info ring, meta;
	struct rt_kfd_queue_desc desc = {0};
	char *abi;

	memset(set, 0, sizeof(*set));
	assert(!rt_kfd_bo_alloc(s, packets * 64, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &set->ring));
	assert(!rt_kfd_bo_alloc(s, 16384, 0, RT_KFD_GTT, RT_KFD_PLACE_WINDOW, &set->meta));
	assert(!rt_kfd_bo_info(s, set->ring, &ring) && !rt_kfd_bo_info(s, set->meta, &meta));
	set->ring_va = ring.va;
	set->meta_va = meta.va;
	set->packets = packets;
	desc.ring = set->ring;
	desc.ring_bytes = packets * 64;
	desc.read_pointer = meta.va + FIXTURE_AQL_READ_ID;
	desc.write_pointer = meta.va + FIXTURE_AQL_WRITE_ID;
	assert(!rt_kfd_queue_create(s, &desc, &set->q));
	assert(!rt_kfd_queue_info(s, set->q, &set->info));
	set->pasid = fixture_pasid_of(meta.va);
	abi = fixture_va_to_host(set->pasid, meta.va, FIXTURE_AQL_QUEUE_BYTES);
	assert(abi);
	*(uint64_t *)(abi + FIXTURE_AQL_RING_BASE) = ring.va;
	*(uint32_t *)(abi + FIXTURE_AQL_RING_SIZE) = packets;
	for (uint32_t i = 0; i < packets; ++i) {
		uint16_t *header = fixture_va_to_host(set->pasid, ring.va + i * 64, 64);

		assert(header);
		*header = FIXTURE_AQL_PACKET_INVALID;
	}
}

static int64_t *signal_value(const struct queue_set *set)
{
	return fixture_va_to_host(set->pasid, set->meta_va + SIGNAL_OFFSET + 8, 8);
}

/* Publish dispatch packet @id (body, then header), advance the write id
 * and ring the doorbell, as an HSA producer does. */
static void submit(struct rt_kfd_session *s, struct queue_set *set, uint64_t id)
{
	char *packet = fixture_va_to_host(set->pasid, set->ring_va + (id % set->packets) * 64, 64);
	uint64_t *write = fixture_va_to_host(set->pasid, set->meta_va + FIXTURE_AQL_WRITE_ID, 8);

	assert(packet && write);
	*(uint64_t *)(packet + FIXTURE_AQL_COMPLETION) = set->meta_va + SIGNAL_OFFSET;
	__atomic_store_n((uint16_t *)packet, FIXTURE_AQL_PACKET_DISPATCH, __ATOMIC_RELEASE);
	__atomic_store_n(write, id + 1, __ATOMIC_RELEASE);
	assert(!rt_kfd_queue_kick(s, set->q, id));
}

static int wait_dispatches(unsigned int count)
{
	for (int i = 0; i < 4000; ++i) {
		if (__atomic_load_n(&cp_dispatches, __ATOMIC_ACQUIRE) >= count)
			return 1;
		usleep(500);
	}
	return 0;
}

int main(void)
{
	struct rt_kfd_session *s, *late = NULL;
	struct rt_power_report r;
	struct queue_set q;
	uint64_t window;
	size_t live_at_start;
	unsigned int removes;

	fixture_device_init();
	fixture_kfd_init();
	live_at_start = kmemcheck_live_bytes();
	fixture_kfd_wq_init();
	fixture_cp_start();
	fixture_restores_allowed = true;
	assert(!rt_kfd_session_supported(adev));

	/* Nothing registered yet: a report, and nothing to resume. */
	assert(!rt_power_kfd_report(adev, &r));
	assert(r.nodes == 1 && r.running == 1 && !r.processes && !r.queues);
	assert(rt_power_kfd_resume(adev, NULL) == -EALREADY);
	assert(!rt_power_kfd_suspended(adev));

	/* ---- a client with a queue doing work ---- */
	assert(!rt_kfd_session_open(adev, &compute_ctx, 4242, "lse", &s));
	window = rt_kfd_session_window_size(s);
	assert(!rt_kfd_session_set_window(s, window * 4, 0));
	make_queue(s, &q, 64);
	assert(mes_adds == 1 && mes_removes == 0);
	*signal_value(&q) = 3;
	submit(s, &q, 0);
	assert(wait_dispatches(1) && *signal_value(&q) == 2);

	/* ---- suspend: kgd2kfd_suspend(kfd, true) ---- */
	assert(!rt_power_kfd_suspend(adev, &r));
	assert(r.nodes == 1 && r.processes == 1 && r.queues == 1);
	assert(!r.active && r.evicted == 1 && !r.running && !r.reset_marked);
	assert(mes_removes == 1 && rt_power_kfd_suspended(adev));
	assert(rt_power_kfd_suspend(adev, NULL) == -EALREADY);
	/* KFD holds new processes off while suspended (kfd_is_locked). */
	assert(rt_kfd_session_open(adev, &compute_ctx, 0, "late", &late) && !late);
	/* Work published now waits: the queue is off MES. */
	submit(s, &q, 1);
	usleep(20000);
	assert(cp_dispatches == 1 && *signal_value(&q) == 2);
	/* The client's memory is untouched by the suspend. */
	assert(kgd_frees == 0 && bo_restores == 0);

	/* ---- resume: kgd2kfd_resume(kfd, true) ---- */
	assert(!rt_power_kfd_resume(adev, &r));
	assert(r.running == 1 && r.queues == 1 && r.active == 1 && !r.evicted);
	assert(!rt_power_kfd_suspended(adev));
	assert(bo_restores == 1);	/* buffers revalidated in place, new eviction fence */
	assert(mes_adds == 2 && mes_doorbells[1] == mes_doorbells[0]);
	/* The paused work continues. */
	assert(wait_dispatches(2) && *signal_value(&q) == 1);
	assert(rt_power_kfd_resume(adev, NULL) == -EALREADY);
	/* And new work runs as before. */
	submit(s, &q, 2);
	assert(wait_dispatches(3) && *signal_value(&q) == 0);

	/* ---- a session closed while suspended ---- */
	assert(!rt_power_kfd_suspend(adev, &r) && mes_removes == 2);
	removes = mes_removes;
	assert(!rt_kfd_session_close(s));
	/* The queue manager is stopped and the queue already off MES: the
	 * teardown issues no REMOVE_QUEUE. */
	assert(mes_removes == removes && kgd_frees == kgd_allocs);
	/* The resume balances upstream's suspend count; KFD takes processes
	 * again. */
	assert(!rt_power_kfd_resume(adev, &r) && r.running == 1 && !r.queues);
	assert(!rt_kfd_session_open(adev, &compute_ctx, 4243, "lse-reload", &s));
	assert(!rt_kfd_session_set_window(s, window * 4, 0));
	make_queue(s, &q, 64);
	*signal_value(&q) = 1;
	submit(s, &q, 0);
	assert(wait_dispatches(4) && *signal_value(&q) == 0);

	/* ---- MES failing a removal on the way down ---- */
	mes_fail_removes = 1;
	assert(rt_power_kfd_suspend(adev, &r) == -EIO);
	assert(mes_failed_removes == 1 && gpu_reset_requests == 1);
	/* Upstream marked the queue inactive before asking MES, so only the
	 * reset mark tells that the queue may still be running. */
	assert(!r.active && r.reset_marked == 1);
	assert(rt_power_kfd_suspended(adev));
	/* Still counted as one suspend: the resume balances it. */
	assert(!rt_power_kfd_resume(adev, &r) && r.active == 1);
	assert(!rt_kfd_session_close(s));

	fixture_kfd_release_processes();
	assert(render_releases == render_opens);
	fixture_cp_stop();
	if (kmemcheck_live_bytes() != live_at_start)
		fprintf(stderr, "kfd_power test: %zu kmalloc bytes live, %zu before the sessions\n",
			kmemcheck_live_bytes(), live_at_start);
	assert(kmemcheck_live_bytes() == live_at_start);
	fixture_kfd_exit();
	fixture_device_fini();
	(void)fixture_report_bos();
	assert(live_bos == 0 && kernel_allocs == 0);
	puts("KFD power over upstream kgd2kfd_suspend/resume: queues unmapped through MES and "
	     "KFD locked while suspended, paused work resumed on the same queue, teardown "
	     "while suspended without MES traffic, and a failed MES removal reported");
	return 0;
}
