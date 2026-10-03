/* DriverKit legacy AQL queues versus KFD/MES HQD and doorbell ownership.
 *
 * Links the real linuxu queue.c, upstream amdgpu_amdkfd_device_init() (which
 * derives KFD's cp_queue_bitmap), KFD's DQM queue accounting, the per-family
 * KFD device queue manager / MQD managers (v11, v12) that build the MQD, and
 * amdgpu's own kernel compute queue MQD builders (gfx_v11_0/gfx_v12_0
 * compute_mqd_init, registered by their set_mqd_funcs) that define what MES
 * MAP_LEGACY_QUEUE receives for a kernel queue. MES packet submission, KFD
 * device init and HDP flushes are mocked. No DriverKit service, MMIO or
 * hardware is used.
 *
 * Geometries: the R9700 (GC 12.0.1, 2 pipes x 4 queues, uni-MES), GC 11.0.0
 * (4 x 4, non-unified MES), the 4 pipes x 8 queues MEC that gfx9/gfx10 and
 * the default gfx11/gfx12 sw_init branches use, and the upstream bounds. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <rt/queue.h>
#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "amdgpu_doorbell.h"
#include "amdgpu_mes.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_mqd_manager.h"
#include "v11_structs.h"
#include "v12_structs.h"

enum layout { NAVI10, MEC_RING0_ONLY };

struct config {
	uint32_t gc;
	bool mes, uni_mes, legacy_queue_map;
	unsigned pipes, queues, kernel_rings;
	enum layout layout;
};

struct fixture {
	struct config cfg;
	struct amdgpu_device adev;
	struct kfd_dev kfd;
	struct kfd_node node;
	struct device_queue_manager dqm;
	struct drm_minor render;
	struct amdgpu_ring sdma;
	uint32_t doorbells[0x2000];
	unsigned long kfd_seen[BITS_TO_LONGS(AMDGPU_MAX_QUEUES)];
	unsigned kfd_inits;
};

/* queue_partition_gfx11.c / queue_partition_gfx12.c: upstream registration
 * and gc register masks. */
void test_gfx_v11_0_set_mqd_funcs(struct amdgpu_device *adev);
void test_gfx_v12_0_set_mqd_funcs(struct amdgpu_device *adev);
extern const uint32_t test_gfx_v11_doorbell_en, test_gfx_v12_doorbell_en;
extern const uint32_t test_gfx_v11_doorbell_bif_drop, test_gfx_v12_doorbell_bif_drop;
extern const uint32_t test_gfx_v11_qswitch_mode, test_gfx_v12_qswitch_mode;
extern const uint32_t test_gfx_v11_kernel_queue, test_gfx_v12_kernel_queue;
extern const uint32_t test_gfx_v11_aql_pq_control, test_gfx_v12_aql_pq_control;
extern const uint32_t test_gfx_v11_tmpring[4], test_gfx_v12_tmpring[4];

static struct mes_map_legacy_queue_input last_map;
static struct mes_unmap_legacy_queue_input last_unmap;
static unsigned maps, unmaps, flushes;
static int map_result;

static int mock_map(struct amdgpu_mes *mes, struct mes_map_legacy_queue_input *in)
{ (void)mes; last_map = *in; maps++; return map_result; }
static int mock_unmap(struct amdgpu_mes *mes, struct mes_unmap_legacy_queue_input *in)
{ (void)mes; last_unmap = *in; unmaps++; return 0; }
static const struct amdgpu_mes_funcs mes_funcs = {
	.map_legacy_queue = mock_map,
	.unmap_legacy_queue = mock_unmap,
};
static const struct amdgpu_ring_funcs compute_funcs = { .type = AMDGPU_RING_TYPE_COMPUTE };
static const struct amdgpu_ring_funcs mes_ring_funcs = { .type = AMDGPU_RING_TYPE_MES };
static const struct amdgpu_ring_funcs gfx_funcs = { .type = AMDGPU_RING_TYPE_GFX };
static const struct amdgpu_ring_funcs sdma_funcs = { .type = AMDGPU_RING_TYPE_SDMA };

/* ---- upstream symbols outside the code under test ---- */
void amdgpu_device_flush_hdp(struct amdgpu_device *adev, struct amdgpu_ring *ring)
{ (void)adev; (void)ring; flushes++; }
void amdgpu_amdkfd_gpuvm_init_mem_limits(void) {}
int amdgpu_device_gpu_recover(struct amdgpu_device *adev, struct amdgpu_job *job,
			      struct amdgpu_reset_context *reset_context)
{ (void)adev; (void)job; (void)reset_context; abort(); }
/* kgd2kfd_device_init() starts by copying the shared resources. */
bool kgd2kfd_device_init(struct kfd_dev *kfd,
			 const struct kgd2kfd_shared_resources *gpu_resources)
{
	struct fixture *f = container_of(kfd, struct fixture, kfd);
	kfd->shared_resources = *gpu_resources;
	memcpy(f->kfd_seen, gpu_resources->cp_queue_bitmap, sizeof(f->kfd_seen));
	f->kfd_inits++;
	return true;
}
/* KFD MQD-manager entry points the AQL path never calls (allocation, HQD
 * register access, HIQ). */
int kfd_gtt_sa_allocate(struct kfd_node *node, unsigned int size,
			struct kfd_mem_obj **mem_obj)
{ (void)node; (void)size; (void)mem_obj; abort(); }
int kfd_gtt_sa_free(struct kfd_node *node, struct kfd_mem_obj *mem_obj)
{ (void)node; (void)mem_obj; abort(); }
void seq_hex_dump(struct seq_file *m, const char *prefix_str, int prefix_type,
		  int rowsize, int groupsize, const void *buf, size_t len, bool ascii)
{ (void)m; (void)prefix_str; (void)prefix_type; (void)rowsize; (void)groupsize;
  (void)buf; (void)len; (void)ascii; abort(); }
/* Kernel-BO, ring and reset helpers reachable only from amdgpu_amdkfd.c
 * paths (HIQ unmap, kernel memory free) the test does not take. */
uint amdgpu_sdma_phase_quantum;
int amdgpu_in_reset(struct amdgpu_device *adev) { (void)adev; return 0; }
void amdgpu_bo_kunmap(struct amdgpu_bo *bo) { (void)bo; abort(); }
void amdgpu_bo_unref(struct amdgpu_bo **bo) { (void)bo; abort(); }
void amdgpu_bo_unpin(struct amdgpu_bo *bo) { (void)bo; abort(); }
int amdgpu_ring_alloc(struct amdgpu_ring *ring, unsigned ndw)
{ (void)ring; (void)ndw; abort(); }
void amdgpu_ring_commit(struct amdgpu_ring *ring) { (void)ring; abort(); }
int amdgpu_ring_test_helper(struct amdgpu_ring *ring) { (void)ring; abort(); }
int dma_resv_lock(struct dma_resv *obj, struct ww_acquire_ctx *ctx)
{ (void)obj; (void)ctx; abort(); }
void dma_resv_unlock(struct dma_resv *obj) { (void)obj; abort(); }
void ttm_bo_move_to_lru_tail(struct ttm_buffer_object *bo) { (void)bo; abort(); }
void *kzalloc_obj1(size_t size) { return calloc(1, size); }
void kfree(const void *p) { free((void *)p); }
/* amdgpu kernel-queue CU masking; kernel rings carry no CU mask. */
void amdgpu_gfx_mqd_symmetrically_map_cu_mask(struct amdgpu_device *adev,
					       const uint32_t *cu_mask,
					       uint32_t cu_mask_count,
					       uint32_t *se_mask)
{ (void)adev; (void)cu_mask; (void)cu_mask_count; (void)se_mask; abort(); }

/* amdgpu_mes_get_hqd_mask() (static in amdgpu_mes.c): MES schedules every
 * queue above DIV_ROUND_UP(kernel rings, pipes) on each pipe. */
static uint32_t mes_hqd_mask(uint32_t pipes, uint32_t queues, uint32_t reserved)
{
	uint32_t total = (uint32_t)((1ULL << queues) - 1);
	return total & ~(uint32_t)((1ULL << DIV_ROUND_UP(reserved, pipes)) - 1);
}

static void add_ring(struct amdgpu_device *adev, struct amdgpu_ring *ring,
		     const struct amdgpu_ring_funcs *funcs, uint32_t me,
		     uint32_t pipe, uint32_t queue, uint32_t doorbell)
{
	ring->adev = adev;
	ring->funcs = funcs;
	ring->me = me; ring->pipe = pipe; ring->queue = queue;
	ring->use_doorbell = true;
	ring->doorbell_index = doorbell;
	ring->idx = adev->num_rings;
	adev->rings[adev->num_rings++] = ring;
}

/* A device after probe: discovery, gfx sw_init geometry, amdgpu_mes_init()'s
 * HQD split, the multipipe kernel compute rings, KFD node and DQM with the
 * per-ASIC MQD managers device_queue_manager_init() selects. */
static struct fixture *device(struct config cfg)
{
	struct fixture *f = calloc(1, sizeof(*f));
	struct amdgpu_device *adev = &f->adev;
	assert(f && cfg.kernel_rings <= AMDGPU_MAX_COMPUTE_RINGS);
	f->cfg = cfg;
	adev->ip_versions[GC_HWIP][0] = cfg.gc;
	adev->enable_mes = cfg.mes;
	adev->enable_uni_mes = cfg.uni_mes;
	adev->mes.enable_legacy_queue_map = cfg.legacy_queue_map;
	adev->mes.funcs = &mes_funcs;
	adev->mes.ring[AMDGPU_MES_SCHED_PIPE].sched.ready = cfg.mes;
	adev->gfx.config.max_shader_engines = 4;
	adev->gfx.config.max_sh_per_se = 2;
	adev->gfx.cu_info.number = 64;
	adev->gfx.cu_info.max_scratch_slots_per_cu = 32;
	adev->gfx.mec.num_mec = 1;
	adev->gfx.mec.num_pipe_per_mec = cfg.pipes;
	adev->gfx.mec.num_queue_per_pipe = cfg.queues;
	adev->gfx.num_compute_rings = cfg.kernel_rings;
	if (cfg.mes)
		for (unsigned p = 0; p < cfg.pipes && p < AMDGPU_MES_MAX_COMPUTE_PIPES; ++p)
			adev->mes.compute_hqd_mask[p] =
				mes_hqd_mask(cfg.pipes, cfg.queues, cfg.kernel_rings);
	adev->vm_manager.first_kfd_vmid = 8;
	adev->vm_manager.max_pfn = 1ULL << 36;
	adev->have_atomics_support = true;
	/* gfx_vN_0_early_init() -> gfx_vN_0_set_mqd_funcs(). */
	if (cfg.gc >= IP_VERSION(12, 0, 0))
		test_gfx_v12_0_set_mqd_funcs(adev);
	else if (cfg.gc >= IP_VERSION(11, 0, 0))
		test_gfx_v11_0_set_mqd_funcs(adev);

	/* nv/soc21/soc24_init_doorbell_index(); soc_v1_0/aqua_vanjaram only
	 * set mec_ring0 and rely on (mec_ring0 + ring_id). */
	adev->doorbell_index.kiq = AMDGPU_NAVI10_DOORBELL_KIQ;
	adev->doorbell_index.mec_ring0 = AMDGPU_NAVI10_DOORBELL_MEC_RING0;
	if (cfg.layout == NAVI10) {
		adev->doorbell_index.mec_ring1 = AMDGPU_NAVI10_DOORBELL_MEC_RING1;
		adev->doorbell_index.mec_ring2 = AMDGPU_NAVI10_DOORBELL_MEC_RING2;
		adev->doorbell_index.mec_ring3 = AMDGPU_NAVI10_DOORBELL_MEC_RING3;
		adev->doorbell_index.mec_ring4 = AMDGPU_NAVI10_DOORBELL_MEC_RING4;
		adev->doorbell_index.mec_ring5 = AMDGPU_NAVI10_DOORBELL_MEC_RING5;
		adev->doorbell_index.mec_ring6 = AMDGPU_NAVI10_DOORBELL_MEC_RING6;
		adev->doorbell_index.mec_ring7 = AMDGPU_NAVI10_DOORBELL_MEC_RING7;
	}
	adev->doorbell_index.userqueue_start = AMDGPU_NAVI10_DOORBELL_USERQUEUE_START;
	adev->doorbell_index.userqueue_end = AMDGPU_NAVI10_DOORBELL_USERQUEUE_END;
	adev->doorbell_index.gfx_ring0 = AMDGPU_NAVI10_DOORBELL_GFX_RING0;
	adev->doorbell_index.mes_ring0 = AMDGPU_NAVI10_DOORBELL_MES_RING0;
	adev->doorbell_index.mes_ring1 = AMDGPU_NAVI10_DOORBELL_MES_RING1;
	adev->doorbell_index.sdma_engine[0] = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0;
	adev->doorbell_index.first_non_cp = AMDGPU_NAVI10_DOORBELL64_FIRST_NON_CP;
	adev->doorbell_index.last_non_cp = AMDGPU_NAVI10_DOORBELL64_LAST_NON_CP;
	/* amdgpu_doorbell_create_kernel_doorbells(): page-aligned kernel range,
	 * then one page for MES kernel doorbells. */
	adev->mes.db_start_dw_offset = PAGE_SIZE / sizeof(u32);
	adev->doorbell.num_kernel_doorbells = 2 * PAGE_SIZE / sizeof(u32);
	assert(adev->doorbell.num_kernel_doorbells <= ARRAY_SIZE(f->doorbells));
	adev->doorbell.cpu_addr = f->doorbells;

	/* amdgpu_gfx_compute_queue_acquire() multipipe policy + the sw_init
	 * ring loop (queue outer, pipe inner) + gfx_vN_0_compute_ring_init(). */
	for (unsigned i = 0; i < cfg.kernel_rings; ++i) {
		unsigned pipe = i % cfg.pipes, queue = (i / cfg.pipes) % cfg.queues;
		set_bit(pipe * cfg.queues + queue, adev->gfx.mec_bitmap[0].queue_bitmap);
		add_ring(adev, &adev->gfx.compute_ring[i], &compute_funcs, 1, pipe,
			 queue, (adev->doorbell_index.mec_ring0 + i) << 1);
	}
	add_ring(adev, &adev->gfx.gfx_ring[0], &gfx_funcs, 0, 0, 0,
		 adev->doorbell_index.gfx_ring0 << 1);
	if (cfg.mes) {
		add_ring(adev, &adev->mes.ring[0], &mes_ring_funcs, 3, 0, 0,
			 adev->doorbell_index.mes_ring0 << 1);
		add_ring(adev, &adev->mes.ring[1], &mes_ring_funcs, 3, 1, 0,
			 adev->doorbell_index.mes_ring1 << 1);
	}
	add_ring(adev, &f->sdma, &sdma_funcs, 0, 0, 0,
		 adev->doorbell_index.sdma_engine[0] << 1);

	f->render.index = 128;
	adev->ddev.render = &f->render;
	adev->kfd.dev = &f->kfd;
	f->kfd.adev = adev;
	f->kfd.cwsr_enabled = true; /* cwsr_enable defaults to 1 */
	f->kfd.num_nodes = 1;
	f->kfd.nodes[0] = &f->node;
	f->node.kfd = &f->kfd;
	f->node.adev = adev;
	f->node.dqm = &f->dqm;
	f->dqm.dev = &f->node;
	/* device_queue_manager_init(): asic_ops by GC version, then
	 * init_mqd_managers() for every MQD type. */
	if (cfg.gc >= IP_VERSION(12, 0, 0))
		device_queue_manager_init_v12(&f->dqm.asic_ops);
	else if (cfg.gc >= IP_VERSION(11, 0, 0))
		device_queue_manager_init_v11(&f->dqm.asic_ops);
	if (f->dqm.asic_ops.mqd_manager_init)
		for (int t = 0; t < KFD_MQD_TYPE_MAX; ++t)
			f->dqm.mqd_mgrs[t] = f->dqm.asic_ops.mqd_manager_init(t, &f->node);
	return f;
}

static void destroy(struct fixture *f)
{
	for (int t = 0; t < KFD_MQD_TYPE_MAX; ++t)
		kfree(f->dqm.mqd_mgrs[t]);
	free(f);
}

static const struct config R9700 = {
	.gc = IP_VERSION(12, 0, 1), .mes = true, .uni_mes = true,
	.legacy_queue_map = true, .pipes = 2, .queues = 4, .kernel_rings = 1,
	.layout = NAVI10,
};
static const struct config NAVI31 = {
	.gc = IP_VERSION(11, 0, 0), .mes = true, .uni_mes = false,
	.legacy_queue_map = true, .pipes = 4, .queues = 4, .kernel_rings = 1,
	.layout = NAVI10,
};
static struct config with(struct config c, unsigned pipes, unsigned queues,
			  unsigned kernel_rings)
{
	c.pipes = pipes; c.queues = queues; c.kernel_rings = kernel_rings;
	return c;
}

static unsigned slot_of(const struct fixture *f, unsigned pipe, unsigned queue)
{ return pipe * f->cfg.queues + queue; }

static int mask_is(const uint64_t *mask, uint64_t w0, uint64_t w1)
{ return mask[0] == w0 && mask[1] == w1; }

static unsigned weight(const uint64_t *mask)
{ return (unsigned)(__builtin_popcountll(mask[0]) + __builtin_popcountll(mask[1])); }

static int mec_bit(struct fixture *f, unsigned slot)
{ return test_bit(slot, f->adev.gfx.mec_bitmap[0].queue_bitmap); }

static void check_exclusive(struct fixture *f, const struct rt_queue_partition *p)
{
	const unsigned pipes = f->cfg.pipes, queues = f->cfg.queues;
	const unsigned slots = pipes * queues;
	assert(p->pipes == pipes && p->queues_per_pipe == queues);
	for (unsigned s = 0; s < RT_QUEUE_MAX_SLOTS; ++s) {
		const int k = rt_queue_mask_test(p->kernel_mask, s);
		const int m = rt_queue_mask_test(p->mes_mask, s);
		const int d = rt_queue_mask_test(p->dext_mask, s);
		if (s >= slots) {
			/* Nothing outside the first MEC is owned or given to KFD. */
			assert(!k && !m && !d && !rt_queue_mask_test(p->kfd_mask, s));
			assert(!p->doorbells[s]);
			continue;
		}
		const unsigned pipe = s / queues, queue = s % queues;
		/* Every first-MEC HQD has exactly one owner, except a legacy HQD
		 * whose kernel ring id would reach AMDGPU_MAX_COMPUTE_RINGS: no
		 * upstream ring (or doorbell) can exist there, so nobody uses it. */
		assert(k + m + d == 1 ||
		       (!k && !m && !d && queue * pipes + pipe >= AMDGPU_MAX_COMPUTE_RINGS));
		/* KFD's cp_queue_bitmap never names a kernel ring or a DriverKit
		 * HQD; with every HQD owned it is exactly what MES schedules. (An
		 * unowned HQD stays in it, as upstream's complement of mec_bitmap
		 * leaves it; MES never schedules it either.) */
		const int unowned = !k && !m && !d;
		assert(rt_queue_mask_test(p->kfd_mask, s) == (m || unowned));
		assert(test_bit(s, f->kfd.shared_resources.cp_queue_bitmap) == (m || unowned));
		/* mec_bitmap records both kinds of kernel-owned legacy queue. */
		assert(mec_bit(f, s) == (k || d));
		assert(!rt_queue_mask_test(p->owned_mask, s) || d);
		/* MES never schedules a DriverKit HQD. */
		assert(!d || !(f->adev.mes.compute_hqd_mask[pipe] & (1u << queue)));
		/* Doorbells: kernel compute ring numbering, inside the MEC range,
		 * unique, outside every ring's 64-bit doorbell and below MES's
		 * kernel page. */
		const uint32_t db = p->doorbells[s];
		if (!d) { assert(!db); continue; }
		const uint32_t ring = queue * pipes + pipe;
		assert(ring < AMDGPU_MAX_COMPUTE_RINGS);
		assert(db == (AMDGPU_NAVI10_DOORBELL_MEC_RING0 + ring) << 1);
		assert(db >= f->adev.doorbell_index.kiq * 2 &&
		       db / 2 <= f->adev.doorbell_index.userqueue_end);
		assert(db + 1 < f->adev.mes.db_start_dw_offset);
		for (unsigned i = 0; i < f->adev.num_rings; ++i) {
			uint32_t other = f->adev.rings[i]->doorbell_index;
			assert(other + 1 < db || other > db + 1);
		}
		for (unsigned other = s + 1; other < RT_QUEUE_MAX_SLOTS; ++other)
			assert(!p->doorbells[other] || p->doorbells[other] != db);
	}
	/* KFD DQM accounting, from the upstream helpers. */
	assert(get_cp_queues_num(&f->dqm) == weight(p->kfd_mask));
	assert(get_pipes_per_mec(&f->dqm) == (int)pipes &&
	       get_queues_per_pipe(&f->dqm) == (int)queues);
	for (unsigned pipe = 0; pipe < pipes; ++pipe) {
		int usable = 0;
		for (unsigned q = 0; q < queues; ++q)
			usable |= rt_queue_mask_test(p->kfd_mask, pipe * queues + q);
		assert(usable); /* every pipe usable by KFD */
	}
}

/* Upstream KFD's compute MQD for the reserved slot, from the device's MQD
 * manager: header, MQD/ring/EOP/pointer addresses, AQL format, doorbell,
 * VMID 0. v11 and v12 share these field offsets. */
#define CHECK_MQD(type, buf, l, db)                                           \
	do {                                                                    \
		const struct type *m = (const void *)(buf);                         \
		assert(m->header == 0xC0310800);                                    \
		assert(m->cp_mqd_base_addr_lo == (uint32_t)(l).mqd_address &&       \
		       m->cp_mqd_base_addr_hi == (uint32_t)((l).mqd_address >> 32)); \
		assert(m->cp_hqd_pq_base_lo == (uint32_t)((l).ring_address >> 8));  \
		assert(m->cp_hqd_pq_rptr_report_addr_lo == (uint32_t)(l).read_pointer); \
		assert(m->cp_hqd_pq_wptr_poll_addr_lo == (uint32_t)(l).write_pointer); \
		assert(m->cp_hqd_eop_base_addr_lo == (uint32_t)((l).eop_address >> 8)); \
		assert(((m->cp_hqd_pq_doorbell_control >> 2) & 0x3ffffff) == (db));  \
		assert(m->cp_hqd_aql_control == 1 && m->cp_hqd_vmid == 0);          \
		assert((m->cp_hqd_pq_control & 0x3f) == 5); /* 2^(5+1) dw = 256 B */ \
	} while (0)

struct gc_masks { uint32_t doorbell_en, bif_drop, qswitch, kernel_queue, aql_pq; };
static struct gc_masks gc_masks(uint32_t gc)
{
	if (gc >= IP_VERSION(12, 0, 0))
		return (struct gc_masks){ test_gfx_v12_doorbell_en, test_gfx_v12_doorbell_bif_drop,
			test_gfx_v12_qswitch_mode, test_gfx_v12_kernel_queue,
			test_gfx_v12_aql_pq_control };
	return (struct gc_masks){ test_gfx_v11_doorbell_en, test_gfx_v11_doorbell_bif_drop,
		test_gfx_v11_qswitch_mode, test_gfx_v11_kernel_queue,
		test_gfx_v11_aql_pq_control };
}

/* KFD's v11/v12 init_mqd: c_queue_debug_en and the atomics capability. */
#define HQ_STATUS0_DEBUG_EN (1u << 14)
#define HQ_STATUS0_ATOMICS (1u << 29)

/* What MES MAP_LEGACY_QUEUE receives for amdgpu's own kernel compute ring on
 * this HQD (amdgpu_ring_to_mqd_prop() + gfx_vN_0_kcq_init_queue()). */
static void amdgpu_kernel_queue_mqd(struct amdgpu_device *adev, const struct rt_queue_mqd *l,
				    uint32_t db, void *out)
{
	struct amdgpu_mqd_prop prop = {
		.mqd_gpu_addr = l->mqd_address, .hqd_base_gpu_addr = l->ring_address,
		.rptr_gpu_addr = l->read_pointer, .wptr_gpu_addr = l->write_pointer,
		.queue_size = l->ring_bytes, .eop_gpu_addr = l->eop_address,
		.use_doorbell = true, .doorbell_index = db, .kernel_queue = true,
		.hqd_active = false,
	};
	memset(out, 0, adev->mqds[AMDGPU_HW_IP_COMPUTE].mqd_size);
	assert(adev->mqds[AMDGPU_HW_IP_COMPUTE].init_mqd(adev, out, &prop) == 0);
}

/* KFD's user-queue compute MQD for the same queue, as the CP manager builds
 * it with kfd->cwsr_enabled: the legacy fields really differ there. */
static void kfd_user_queue_mqd(struct fixture *f, const struct rt_queue_mqd *l,
			       uint32_t db, void *out)
{
	struct mqd_manager *mm = f->dqm.mqd_mgrs[KFD_MQD_TYPE_CP];
	struct queue_properties q = {
		.type = KFD_QUEUE_TYPE_COMPUTE, .format = KFD_QUEUE_FORMAT_AQL,
		.queue_address = l->ring_address, .queue_size = l->ring_bytes,
		.queue_percent = 100,
		.read_ptr = (void *)(uintptr_t)l->read_pointer,
		.write_ptr = (void *)(uintptr_t)l->write_pointer,
		.doorbell_off = db, .eop_ring_buffer_address = l->eop_address,
		.eop_ring_buffer_size = l->eop_bytes,
	};
	struct kfd_mem_obj obj = { .gpu_addr = l->mqd_address, .cpu_ptr = out };
	void *m = NULL;
	uint64_t gart = 0;
	mm->init_mqd(mm, &m, &obj, &gart, &q);
	assert(m == out);
}

/* The MQD rt_queue_build_mqd() wrote is a legacy kernel queue: doorbell
 * enabled exactly as amdgpu enables it (plus KFD's AQL BIF_DROP), inactive,
 * no CWSR, no debugger, privileged kernel queue, AQL format intact, atomics
 * following amdgpu_amdkfd_have_atomics_support(). u is KFD's user-queue MQD
 * for the same queue, r amdgpu's kernel-queue MQD. */
#define CHECK_LEGACY(type, buf, user_buf, ref_buf, mk, atomics)                 \
	do {                                                                    \
		const struct type *m = (const void *)(buf);                         \
		const struct type *u = (const void *)(user_buf);                    \
		const struct type *r = (const void *)(ref_buf);                     \
		/* The fields KFD's user-queue MQD sets differently. */             \
		assert(!(u->cp_hqd_pq_doorbell_control & (mk).doorbell_en));        \
		assert(u->cp_hqd_persistent_state & (mk).qswitch);                  \
		assert(u->cp_hqd_hq_status0 & HQ_STATUS0_DEBUG_EN);                 \
		assert(!(u->cp_hqd_pq_control & (mk).kernel_queue));                \
		/* Doorbell. */                                                     \
		assert(r->cp_hqd_pq_doorbell_control & (mk).doorbell_en);           \
		assert(m->cp_hqd_pq_doorbell_control & (mk).doorbell_en);           \
		assert(m->cp_hqd_pq_doorbell_control ==                             \
		       (r->cp_hqd_pq_doorbell_control | (mk).bif_drop));            \
		/* Activation is MAP_LEGACY_QUEUE's job, as for amdgpu's rings. */  \
		assert(m->cp_hqd_active == 0 && r->cp_hqd_active == 0);             \
		/* No CWSR, despite kfd->cwsr_enabled. */                           \
		assert(!(m->cp_hqd_persistent_state & (mk).qswitch));               \
		assert(m->cp_hqd_persistent_state == r->cp_hqd_persistent_state);   \
		assert(!m->cp_hqd_ctx_save_base_addr_lo &&                          \
		       !m->cp_hqd_ctx_save_base_addr_hi && !m->cp_hqd_ctx_save_size && \
		       !m->cp_hqd_ctx_save_control && !m->cp_hqd_cntl_stack_size && \
		       !m->cp_hqd_cntl_stack_offset && !m->cp_hqd_wg_state_offset); \
		/* Kernel queue privilege, exactly amdgpu's. */                     \
		assert((r->cp_hqd_pq_control & (mk).kernel_queue) == (mk).kernel_queue); \
		assert((m->cp_hqd_pq_control & (mk).kernel_queue) == (mk).kernel_queue); \
		/* hq_status0: no debugger (amdgpu leaves it 0); atomics from       \
		 * upstream's have_atomics_support. */                             \
		assert(r->cp_hqd_hq_status0 == 0);                                  \
		assert(m->cp_hqd_hq_status0 == ((atomics) ? HQ_STATUS0_ATOMICS : 0)); \
		/* AQL setup untouched. */                                          \
		assert((m->cp_hqd_pq_control & (mk).aql_pq) ==                      \
		       (u->cp_hqd_pq_control & (mk).aql_pq));                       \
		assert((m->cp_hqd_pq_control & (mk).aql_pq) && m->cp_hqd_aql_control == 1); \
		assert(m->cp_hqd_eop_control == u->cp_hqd_eop_control &&            \
		       m->cp_hqd_quantum == u->cp_hqd_quantum);                     \
	} while (0)

static void build_and_map(struct fixture *f, uint32_t slot, uint32_t db,
			  unsigned pipe, unsigned queue)
{
	struct amdgpu_device *adev = &f->adev;
	struct rt_queue_geometry g;
	static uint32_t mqd[4096 / 4];
	struct rt_queue_mqd l = {
		.mqd_address = 0x100000000ULL, .ring_address = 0x100002000ULL,
		.read_pointer = 0x100003080ULL, .write_pointer = 0x100003088ULL,
		.eop_address = 0x100001000ULL, .ring_bytes = 256, .eop_bytes = 4096,
	};
	assert(rt_queue_geometry(adev, &g) == 0);
	assert(g.gfx_major == IP_VERSION_MAJ(f->cfg.gc) &&
	       g.gfx_minor == IP_VERSION_MIN(f->cfg.gc) &&
	       g.gfx_revision == IP_VERSION_REV(f->cfg.gc));
	assert(g.engines == 4 && g.compute_units == 64 && g.waves_per_cu == 32);
	/* Scratch encoding: COMPUTE_TMPRING_SIZE as the family's gc header
	 * (the one gfx_vN_0.c builds with) defines it. */
	const uint32_t *tr = f->cfg.gc >= IP_VERSION(12, 0, 0) ? test_gfx_v12_tmpring :
				test_gfx_v11_tmpring;
	assert(g.tmpring.waves_mask == tr[0] && g.tmpring.waves_shift == tr[1] &&
	       g.tmpring.wave_size_mask == tr[2] && g.tmpring.wave_size_shift == tr[3]);
	assert(g.mqd_bytes == AMDGPU_MQD_SIZE_ALIGN(f->dqm.mqd_mgrs[KFD_MQD_TYPE_HIQ]->mqd_size));
	assert(g.mqd_bytes <= sizeof(mqd));

	/* Only the reserved slot with its own doorbell, and sane addresses. */
	assert(rt_queue_build_mqd(adev, slot, db + 2, &l, mqd, sizeof(mqd)) == -EINVAL);
	assert(rt_queue_build_mqd(adev, slot ^ 1, db, &l, mqd, sizeof(mqd)) == -EINVAL);
	assert(rt_queue_build_mqd(adev, slot, db, &l, mqd, g.mqd_bytes - 4) == -EOPNOTSUPP);
	l.ring_address += 4;
	assert(rt_queue_build_mqd(adev, slot, db, &l, mqd, sizeof(mqd)) == -EINVAL);
	l.ring_address -= 4; l.ring_bytes = 384;
	assert(rt_queue_build_mqd(adev, slot, db, &l, mqd, sizeof(mqd)) == -EINVAL);
	l.ring_bytes = 256;
	static uint32_t user[4096 / 4], ref[4096 / 4];
	const struct gc_masks mk = gc_masks(f->cfg.gc);
	kfd_user_queue_mqd(f, &l, db, user);
	amdgpu_kernel_queue_mqd(adev, &l, db, ref);
	for (int atomics = 1; atomics >= 0; --atomics) {
		adev->have_atomics_support = atomics;
		memset(mqd, 0xa5, sizeof(mqd));
		assert(rt_queue_build_mqd(adev, slot, db, &l, mqd, sizeof(mqd)) == 0);
		if (f->cfg.gc >= IP_VERSION(12, 0, 0)) {
			CHECK_MQD(v12_compute_mqd, mqd, l, db);
			CHECK_LEGACY(v12_compute_mqd, mqd, user, ref, mk, atomics);
		} else {
			CHECK_MQD(v11_compute_mqd, mqd, l, db);
			CHECK_LEGACY(v11_compute_mqd, mqd, user, ref, mk, atomics);
		}
	}
	adev->have_atomics_support = true;

	maps = unmaps = 0;
	assert(rt_queue_map(adev, slot, db + 2, l.mqd_address, l.write_pointer) == -EINVAL);
	assert(maps == 0);
	assert(rt_queue_map(adev, slot, db, l.mqd_address, l.write_pointer) == 0 && maps == 1);
	assert(last_map.queue_type == AMDGPU_RING_TYPE_COMPUTE &&
	       last_map.pipe_id == pipe && last_map.queue_id == queue &&
	       last_map.doorbell_offset == db && last_map.mqd_addr == l.mqd_address &&
	       last_map.wptr_addr == l.write_pointer);
	assert(rt_queue_kick(adev, db, 0x1122334455667788ULL) == 0);
	assert(*(uint64_t *)&f->doorbells[db] == 0x1122334455667788ULL);
	assert(rt_queue_unmap(adev, slot, db) == 0 && unmaps == 1);
	assert(last_unmap.pipe_id == pipe && last_unmap.queue_id == queue &&
	       last_unmap.doorbell_offset == db && last_unmap.action == PREEMPT_QUEUES);
}

static void r9700_probe_order_kfd_then_partition(void)
{
	struct fixture *f = device(R9700);
	struct amdgpu_device *adev = &f->adev;
	struct rt_queue_partition p;
	uint32_t slot = 99, db = 0, again = 0, again_db = 0;

	/* Real probe order: KFD is initialized inside amdgpu_device_init(). */
	amdgpu_amdkfd_device_init(adev);
	assert(f->kfd_inits == 1 && f->kfd_seen[0] == 0xfe);
	/* MES schedules queues 1-3 of both pipes (compute_hqd_mask 0xe). The
	 * former "first free mec_bitmap slot" policy picked HQD 1 = pipe 0
	 * queue 1, which MES hands to KFD queues. */
	assert(adev->mes.compute_hqd_mask[0] == 0xe && adev->mes.compute_hqd_mask[1] == 0xe);

	assert(rt_queue_partition(adev, &p) == 0);
	assert(mask_is(p.kernel_mask, 0x01, 0) && mask_is(p.mes_mask, 0xee, 0) &&
	       mask_is(p.dext_mask, 0x10, 0));
	assert(p.doorbells[4] == AMDGPU_NAVI10_DOORBELL_MEC_RING1 << 1);
	check_exclusive(f, &p);

	/* KFD re-deriving its resources sees the same split. */
	amdgpu_amdkfd_device_init(adev);
	assert(f->kfd_inits == 2 && f->kfd_seen[0] == 0xee);
	assert(rt_queue_partition(adev, &p) == 0 && mask_is(p.dext_mask, 0x10, 0));
	check_exclusive(f, &p);

	/* The only DriverKit HQD is pipe 1 queue 0, outside MES's mask. */
	assert(rt_queue_reserve(adev, &slot, &db) == 0 && slot == 4 &&
	       db == AMDGPU_NAVI10_DOORBELL_MEC_RING1 << 1);
	assert(rt_queue_reserve(adev, &again, &again_db) == -ENOSPC);
	assert(rt_queue_partition(adev, &p) == 0 && mask_is(p.owned_mask, 0x10, 0));
	check_exclusive(f, &p);

	assert(rt_queue_map(adev, 1, AMDGPU_NAVI10_DOORBELL_MEC_RING0 << 1, 0x1000, 0x2000) == -EINVAL);
	/* Kicks are confined to the reserved doorbell. */
	assert(rt_queue_kick(adev, AMDGPU_NAVI10_DOORBELL_MEC_RING0 << 1, 7) == -EINVAL);
	build_and_map(f, slot, db, 1, 0);
	assert(f->doorbells[AMDGPU_NAVI10_DOORBELL_MEC_RING0 << 1] == 0);

	/* Release returns the HQD and doorbell; the partition itself stays. */
	rt_queue_release(adev, slot);
	assert(rt_queue_partition(adev, &p) == 0 && mask_is(p.owned_mask, 0, 0) &&
	       mask_is(p.dext_mask, 0x10, 0));
	assert(rt_queue_map(adev, slot, db, 0x1000, 0x2000) == -EINVAL);
	assert(rt_queue_kick(adev, db, 1) == -EINVAL);
	check_exclusive(f, &p);
	assert(rt_queue_reserve(adev, &again, &again_db) == 0 && again == 4 && again_db == db);
	rt_queue_release(adev, again);
	destroy(f);
}

static void r9700_partition_before_kfd_init(void)
{
	struct fixture *f = device(R9700);
	struct rt_queue_partition p;
	/* A different device may take over once the previous one owns nothing. */
	assert(rt_queue_partition(&f->adev, &p) == 0 && mask_is(p.dext_mask, 0x10, 0));
	amdgpu_amdkfd_device_init(&f->adev);
	assert(f->kfd_seen[0] == 0xee);
	assert(rt_queue_partition(&f->adev, &p) == 0);
	check_exclusive(f, &p);
	destroy(f);
}

static void r9700_wider_legacy_range(void)
{
	/* If MES were told to leave two queues per pipe to the kernel (as with
	 * 3-4 kernel rings), one kernel ring leaves three DriverKit HQDs. */
	struct fixture *f = device(R9700);
	struct rt_queue_partition p;
	uint32_t slots[4], dbs[4];
	f->adev.mes.compute_hqd_mask[0] = f->adev.mes.compute_hqd_mask[1] =
		mes_hqd_mask(2, 4, 4);
	amdgpu_amdkfd_device_init(&f->adev);
	assert(rt_queue_partition(&f->adev, &p) == 0);
	assert(mask_is(p.kernel_mask, 0x01, 0) && mask_is(p.mes_mask, 0xcc, 0) &&
	       mask_is(p.dext_mask, 0x32, 0));
	assert(p.doorbells[1] == AMDGPU_NAVI10_DOORBELL_MEC_RING2 << 1);
	assert(p.doorbells[4] == AMDGPU_NAVI10_DOORBELL_MEC_RING1 << 1);
	assert(p.doorbells[5] == AMDGPU_NAVI10_DOORBELL_MEC_RING3 << 1);
	check_exclusive(f, &p);
	for (unsigned i = 0; i < 3; ++i)
		assert(rt_queue_reserve(&f->adev, &slots[i], &dbs[i]) == 0 &&
		       p.doorbells[slots[i]] == dbs[i]);
	assert(rt_queue_reserve(&f->adev, &slots[3], &dbs[3]) == -ENOSPC);
	assert(rt_queue_partition(&f->adev, &p) == 0 && mask_is(p.owned_mask, 0x32, 0));
	check_exclusive(f, &p);
	for (unsigned i = 0; i < 3; ++i) rt_queue_release(&f->adev, slots[i]);
	assert(rt_queue_partition(&f->adev, &p) == 0 && mask_is(p.owned_mask, 0, 0));
	destroy(f);
}

/* The legacy range is entirely consumed by kernel rings: refuse rather than
 * share, and leave mec_bitmap and KFD's cp_queue_bitmap untouched. */
static void legacy_range_consumed(struct config cfg)
{
	struct fixture *f = device(cfg);
	unsigned long before_mec[BITS_TO_LONGS(AMDGPU_MAX_COMPUTE_QUEUES)];
	uint32_t slot, db;
	amdgpu_amdkfd_device_init(&f->adev);
	memcpy(before_mec, f->adev.gfx.mec_bitmap[0].queue_bitmap, sizeof(before_mec));
	assert(rt_queue_partition(&f->adev, NULL) == -ENOSPC);
	assert(rt_queue_reserve(&f->adev, &slot, &db) == -ENOSPC);
	assert(!memcmp(before_mec, f->adev.gfx.mec_bitmap[0].queue_bitmap, sizeof(before_mec)));
	assert(!memcmp(f->kfd_seen, f->kfd.shared_resources.cp_queue_bitmap, sizeof(f->kfd_seen)));
	for (unsigned s = 0; s < cfg.pipes * cfg.queues; ++s) {
		const unsigned pipe = s / cfg.queues, queue = s % cfg.queues;
		const int mes = !!(f->adev.mes.compute_hqd_mask[pipe] & (1u << queue));
		assert(test_bit(s, f->kfd.shared_resources.cp_queue_bitmap) == mes);
		assert(mec_bit(f, s) == !mes); /* the legacy range is all kernel rings */
	}
	destroy(f);
}

/* Partition and reserve every DriverKit HQD on a geometry; expect exactly
 * the HQDs listed (pipe, queue), each with its kernel ring doorbell. */
static void geometry_case(struct config cfg, unsigned n, const unsigned (*hqd)[2])
{
	struct fixture *f = device(cfg);
	struct rt_queue_partition p;
	uint64_t expect[RT_QUEUE_MASK_WORDS] = {0};
	uint32_t slots[RT_QUEUE_MAX_SLOTS], dbs[RT_QUEUE_MAX_SLOTS];
	amdgpu_amdkfd_device_init(&f->adev);
	assert(rt_queue_partition(&f->adev, &p) == 0);
	for (unsigned i = 0; i < n; ++i) {
		const unsigned s = slot_of(f, hqd[i][0], hqd[i][1]);
		expect[s / 64] |= 1ULL << (s % 64);
		assert(p.doorbells[s] == (AMDGPU_NAVI10_DOORBELL_MEC_RING0 +
					  hqd[i][1] * cfg.pipes + hqd[i][0]) << 1);
	}
	assert(mask_is(p.dext_mask, expect[0], expect[1]));
	check_exclusive(f, &p);
	for (unsigned i = 0; i < n; ++i) {
		assert(rt_queue_reserve(&f->adev, &slots[i], &dbs[i]) == 0);
		assert(rt_queue_mask_test(expect, slots[i]) && dbs[i] == p.doorbells[slots[i]]);
	}
	assert(rt_queue_reserve(&f->adev, &slots[n], &dbs[n]) == -ENOSPC);
	assert(rt_queue_partition(&f->adev, &p) == 0 &&
	       mask_is(p.owned_mask, expect[0], expect[1]));
	check_exclusive(f, &p);
	/* Every reserved HQD builds its MQD with the device's KFD MQD manager
	 * and maps at its own pipe/queue. */
	for (unsigned i = 0; i < n; ++i)
		build_and_map(f, slots[i], dbs[i], slots[i] / cfg.queues,
			      slots[i] % cfg.queues);
	for (unsigned i = 0; i < n; ++i)
		rt_queue_release(&f->adev, slots[i]);
	destroy(f);
}

static void navi31_and_four_by_eight(void)
{
	/* GC 11.0.0: 4 x 4, non-unified MES, KFD v11 MQD manager. */
	static const unsigned navi31_1[][2] = { {1, 0}, {2, 0}, {3, 0} };
	static const unsigned navi31_3[][2] = { {3, 0} };
	geometry_case(NAVI31, 3, navi31_1);          /* several spare */
	geometry_case(with(NAVI31, 4, 4, 3), 1, navi31_3); /* one spare */
	legacy_range_consumed(with(NAVI31, 4, 4, 4)); /* zero spare */

	/* 4 pipes x 8 queues (gfx9/gfx10 MEC; gfx11/gfx12 sw_init default). */
	const struct config g4x8 = with(NAVI31, 4, 8, 1);
	static const unsigned four8_1[][2] = { {1, 0}, {2, 0}, {3, 0} };
	static const unsigned four8_3[][2] = { {3, 0} };
	static const unsigned four8_5[][2] = { {1, 1}, {2, 1}, {3, 1} };
	static const unsigned four8_7[][2] = { {3, 1} };
	geometry_case(g4x8, 3, four8_1);
	geometry_case(with(g4x8, 4, 8, 3), 1, four8_3);
	legacy_range_consumed(with(g4x8, 4, 8, 4));
	geometry_case(with(g4x8, 4, 8, 5), 3, four8_5);
	geometry_case(with(g4x8, 4, 8, 7), 1, four8_7);
	legacy_range_consumed(with(g4x8, 4, 8, 8));
	/* The same 4 x 8 MEC on a gfx12 device (v12 MQD manager). */
	geometry_case(with(R9700, 4, 8, 1), 3, four8_1);
	/* R9700 with two kernel rings fills both legacy HQDs. */
	legacy_range_consumed(with(R9700, 2, 4, 2));
}

static void upstream_bounds(void)
{
	/* The largest geometry the upstream structures describe: 8 pipes (MES
	 * compute_hqd_mask[]) x 16 queues = 128 (mec_bitmap queue_bitmap). One
	 * kernel ring leaves pipes 1-7 queue 0, slots crossing mask word 0. */
	static const unsigned big[][2] = {
		{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0},
	};
	geometry_case(with(NAVI31, 8, 16, 1), 7, big);

	struct fixture *f = device(with(NAVI31, 8, 16, 1));
	uint32_t slot, db;
	f->adev.gfx.mec.num_queue_per_pipe = 32;   /* 256 > 128 HQDs */
	assert(rt_queue_partition(&f->adev, NULL) == -EOPNOTSUPP);
	assert(rt_queue_reserve(&f->adev, &slot, &db) == -EOPNOTSUPP);
	f->adev.gfx.mec.num_pipe_per_mec = 9;      /* > AMDGPU_MES_MAX_COMPUTE_PIPES */
	f->adev.gfx.mec.num_queue_per_pipe = 4;
	assert(rt_queue_partition(&f->adev, NULL) == -EOPNOTSUPP);
	f->adev.gfx.mec.num_pipe_per_mec = 2;
	f->adev.gfx.mec.num_queue_per_pipe = 33;   /* > compute_hqd_mask bits */
	assert(rt_queue_partition(&f->adev, NULL) == -EOPNOTSUPP);
	f->adev.gfx.mec.num_mec = 0;
	f->adev.gfx.mec.num_queue_per_pipe = 4;
	assert(rt_queue_partition(&f->adev, NULL) == -EOPNOTSUPP);
	destroy(f);
}

static void capability_gates(void)
{
	uint32_t slot, db;
	struct rt_queue_geometry g;
	/* No MES legacy-queue mapping (e.g. gfx9/gfx10, or gfx11 MES firmware
	 * older than 0x47): -ENODEV from the capability, whatever the GC. */
	struct config gfx10 = with(NAVI31, 4, 8, 1);
	gfx10.gc = IP_VERSION(10, 3, 0);
	gfx10.mes = gfx10.legacy_queue_map = false;
	struct fixture *f = device(gfx10);
	assert(rt_queue_geometry(&f->adev, &g) == -ENODEV);
	assert(rt_queue_reserve(&f->adev, &slot, &db) == -ENODEV);
	destroy(f);
	struct config old_fw = NAVI31;
	old_fw.legacy_queue_map = false;
	f = device(old_fw);
	assert(rt_queue_geometry(&f->adev, &g) == -ENODEV);
	assert(rt_queue_reserve(&f->adev, &slot, &db) == -ENODEV);
	destroy(f);
	/* No KFD kernel-queue MQD manager: nothing to build the MQD with. */
	f = device(R9700);
	f->dqm.mqd_mgrs[KFD_MQD_TYPE_HIQ]->mqd_size = 0;
	assert(rt_queue_geometry(&f->adev, &g) == -ENODEV);
	destroy(f);
	/* No amdgpu kernel compute queue MQD builder, or one for another MQD
	 * layout: the legacy fields cannot be derived. */
	f = device(R9700);
	f->adev.mqds[AMDGPU_HW_IP_COMPUTE].init_mqd = NULL;
	assert(rt_queue_geometry(&f->adev, &g) == -ENODEV);
	destroy(f);
	f = device(NAVI31);
	f->adev.mqds[AMDGPU_HW_IP_COMPUTE].mqd_size += 4;
	assert(rt_queue_geometry(&f->adev, &g) == -ENODEV);
	destroy(f);
	/* More than one XCC: the partition covers XCC 0 only. */
	f = device(R9700);
	f->adev.gfx.xcc_mask = 0x3;
	assert(rt_queue_geometry(&f->adev, &g) == -EOPNOTSUPP);
	destroy(f);
	/* Kernel rings that do not follow the multipipe numbering (as with
	 * amdgpu_compute_multipipe=0) break the doorbell derivation. */
	f = device(with(NAVI31, 4, 8, 2));
	f->adev.gfx.compute_ring[1].pipe = 0;
	f->adev.gfx.compute_ring[1].queue = 1;
	assert(rt_queue_partition(&f->adev, NULL) == -EOPNOTSUPP);
	destroy(f);
}

static void ring_id_cap(void)
{
	/* 3 pipes x 4 queues with 7 kernel rings: the legacy range is three
	 * queues per pipe (9 HQDs). Pipe 1 queue 2 is ring 7 (mec_ring7); pipe 2
	 * queue 2 would be ring 8, past AMDGPU_MAX_COMPUTE_RINGS, so it gets no
	 * doorbell even where the slot after mec_ring7 is free (vi leaves
	 * 0x18-0x1f unused): move the MES rings away to expose that case. */
	struct config c = with(NAVI31, 3, 4, 7);
	struct fixture *f = device(c);
	struct rt_queue_partition p;
	f->adev.mes.ring[0].doorbell_index = 0x100;
	f->adev.mes.ring[1].doorbell_index = 0x102;
	amdgpu_amdkfd_device_init(&f->adev);
	assert(rt_queue_partition(&f->adev, &p) == 0);
	assert(mask_is(p.dext_mask, 1ULL << slot_of(f, 1, 2), 0));
	assert(p.doorbells[slot_of(f, 1, 2)] == AMDGPU_NAVI10_DOORBELL_MEC_RING7 << 1);
	assert(!p.doorbells[slot_of(f, 2, 2)] && !mec_bit(f, slot_of(f, 2, 2)));
	check_exclusive(f, &p);
	destroy(f);
}

static void mec_ring0_only_layout(void)
{
	/* soc_v1_0 / aqua_vanjaram set only mec_ring0; ring doorbells are
	 * (mec_ring0 + ring_id) << 1 there as everywhere else. */
	struct config c = with(NAVI31, 4, 4, 1);
	c.layout = MEC_RING0_ONLY;
	static const unsigned hqd[][2] = { {1, 0}, {2, 0}, {3, 0} };
	geometry_case(c, 3, hqd);
}

static void doorbell_collision_guard(void)
{
	/* A ring already holding the HQD's kernel doorbell blocks that HQD. */
	struct fixture *f = device(R9700);
	struct amdgpu_ring squatter = {0};
	add_ring(&f->adev, &squatter, &sdma_funcs, 0, 0, 0,
		 (AMDGPU_NAVI10_DOORBELL_MEC_RING1 << 1) + 1);
	amdgpu_amdkfd_device_init(&f->adev);
	assert(rt_queue_partition(&f->adev, NULL) == -ENOSPC);
	assert(f->kfd.shared_resources.cp_queue_bitmap[0] == 0xfe);
	destroy(f);
}

static void busy_device(void)
{
	struct fixture *a = device(R9700), *b = device(R9700);
	uint32_t slot, db, other, other_db;
	amdgpu_amdkfd_device_init(&a->adev);
	assert(rt_queue_reserve(&a->adev, &slot, &db) == 0);
	assert(rt_queue_reserve(&b->adev, &other, &other_db) == -EBUSY);
	assert(rt_queue_partition(&b->adev, NULL) == -EBUSY);
	rt_queue_release(&a->adev, slot);
	assert(rt_queue_reserve(&b->adev, &other, &other_db) == 0 && other == 4);
	rt_queue_release(&b->adev, other);
	b->adev.mes.ring[AMDGPU_MES_SCHED_PIPE].sched.ready = false;
	assert(rt_queue_reserve(&b->adev, &other, &other_db) == -ENODEV);
	destroy(a);
	destroy(b);
}

int main(void)
{
	r9700_probe_order_kfd_then_partition();
	r9700_partition_before_kfd_init();
	r9700_wider_legacy_range();
	navi31_and_four_by_eight();
	upstream_bounds();
	capability_gates();
	ring_id_cap();
	mec_ring0_only_layout();
	doorbell_collision_guard();
	busy_device();
	puts("queue partition: DriverKit HQDs/doorbells disjoint from KFD, MES and kernel rings "
	     "on 2x4, 4x4, 4x8 and 8x16 MECs; MQDs from KFD's v11/v12 managers with "
	     "amdgpu's legacy kernel queue fields");
	return 0;
}
