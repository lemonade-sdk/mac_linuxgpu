/* rt_device_topology(): the HSA runtime's compute topology and product name
 * come from upstream KFD topology node/cache properties, KFD's XNACK mode
 * selection (real kfd_process_xnack_mode()) and amdgpu's FRU product name.
 * The topology device lookup is mocked; no hardware is used. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include <rt/compute.h>
#include "amdgpu.h"
#include "amdgpu_fru_eeprom.h"
#include "kfd_priv.h"
#include "kfd_topology.h"

static struct kfd_topology_device topo;
struct kfd_topology_device *kfd_topology_device_by_id(uint32_t gpu_id)
{ return gpu_id == topo.gpu_id ? &topo : NULL; }
void *kzalloc(size_t size, gfp_t flags) { (void)flags; return calloc(1, size); }
/* Reached only for per-process XNACK with supported == true. */
bool amdgpu_sriov_xnack_support(struct amdgpu_device *adev) { (void)adev; abort(); }
void kfree(const void *p) { free((void *)p); }
/* Not a virtual function here: never reached. */
u32 amdgpu_sriov_rreg(struct amdgpu_device *adev, u32 offset, u32 acc_flags, u32 hwip, u32 xcc_id)
{ (void)adev; (void)offset; (void)acc_flags; (void)hwip; (void)xcc_id; abort(); }
/* The GC registers the device spec reads (read only): GC segment bases
 * 0x1000 and 0xa000 here; anything else is a register it must not read. */
static unsigned reg_reads;
uint32_t amdgpu_device_rreg(struct amdgpu_device *adev, uint32_t reg, uint32_t acc_flags)
{
	(void)adev; (void)acc_flags;
	++reg_reads;
	if (reg == 0x1000 + 0x0fe9) return 0x00030000;	/* GRBM_CC_GC_SA_UNIT_DISABLE */
	if (reg == 0xa000 + 0x5b92) return 0x00040000;	/* GRBM_GC_USER_SA_UNIT_DISABLE */
	abort();
}

static struct kfd_cache_properties caches[4];
static void add_cache(unsigned i, uint32_t level, uint32_t kb, uint32_t type)
{
	caches[i].cache_level = level;
	caches[i].cache_size = kb;
	caches[i].cache_type = type;
	list_add_tail(&caches[i].list, &topo.cache_props);
}

int main(void)
{
	static struct amdgpu_device adev;
	static struct kfd_dev kfd;
	static struct kfd_node node0, node1;
	static struct amdgpu_fru_info fru;
	struct rt_compute_topology t;

	adev.kfd.dev = &kfd;
	kfd.adev = &adev;
	kfd.num_nodes = 1;
	kfd.nodes[0] = &node0;
	node0.kfd = &kfd;
	node0.adev = &adev;
	node0.id = 0x1234;
	node0.xcc_mask = 0x1;
	adev.ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	topo.gpu_id = 0x1234;
	topo.gpu = &node0;
	INIT_LIST_HEAD(&topo.cache_props);
	topo.node_props.gfx_target_version = 120001;
	topo.node_props.simd_per_cu = 2;
	topo.node_props.max_waves_per_simd = 16;
	topo.node_props.lds_size_in_kb = 64;
	topo.node_props.max_slots_scratch_cu = 32;
	topo.node_props.max_engine_clk_fcompute = 2920;
	topo.node_props.capability = HSA_CAP_SRAM_EDCSUPPORTED;
	/* Instruction L1 first: only data caches count. */
	add_cache(0, 1, 64, HSA_CACHE_TYPE_INSTRUCTION | HSA_CACHE_TYPE_HSACU);
	add_cache(1, 1, 32, HSA_CACHE_TYPE_DATA | HSA_CACHE_TYPE_HSACU);
	add_cache(2, 2, 8192, HSA_CACHE_TYPE_DATA | HSA_CACHE_TYPE_HSACU);
	add_cache(3, 3, 65536, HSA_CACHE_TYPE_DATA | HSA_CACHE_TYPE_HSACU);

	/* KFD not initialized, or the node is not in its topology. */
	assert(rt_device_topology(&adev, &t) == -ENODEV);
	kfd.init_complete = true;
	topo.gpu = &node1;
	assert(rt_device_topology(&adev, &t) == -ENODEV);
	topo.gpu = &node0;
	assert(rt_device_topology(NULL, &t) == -EINVAL);

	memset(&t, 0xa5, sizeof(t));
	assert(rt_device_topology(&adev, &t) == 0);
	assert(t.gfx_target_version == 120001 && t.simd_per_cu == 2 &&
	       t.max_waves_per_simd == 16 && t.lds_bytes == 65536 &&
	       t.scratch_slots_per_cu == 32 && t.max_engine_clock_mhz == 2920);
	assert(t.l1_bytes == 32 * 1024 && t.l2_bytes == 8192 * 1024ull &&
	       t.l3_bytes == 65536 * 1024ull);
	/* GC 10.1.1 - 12.0.x: KFD never selects XNACK. */
	assert(t.xnack == RT_TARGET_FEATURE_OFF);
	assert(t.sramecc == RT_TARGET_FEATURE_ON);
	assert(t.xcc_count == 1);
	for (unsigned i = 0; i < sizeof(t.product_name); ++i)
		assert(!t.product_name[i]); /* no FRU EEPROM */

	/* A board FRU name; SRAM EDC off; no L3. */
	strcpy(fru.product_name, "AMD Radeon Test");
	adev.fru_info = &fru;
	topo.node_props.capability = 0;
	list_del(&caches[3].list);
	assert(rt_device_topology(&adev, &t) == 0);
	assert(!strcmp(t.product_name, "AMD Radeon Test"));
	assert(t.sramecc == RT_TARGET_FEATURE_OFF && t.l3_bytes == 0);

	/* GFX9 with retry enabled: KFD defaults to XNACK on; noretry turns it
	 * off. Pre-GFX9 has no XNACK mode. */
	adev.ip_versions[GC_HWIP][0] = IP_VERSION(9, 4, 2);
	kfd.noretry = 0;
	assert(rt_device_topology(&adev, &t) == 0 && t.xnack == RT_TARGET_FEATURE_ON);
	kfd.noretry = 1;
	assert(rt_device_topology(&adev, &t) == 0 && t.xnack == RT_TARGET_FEATURE_OFF);
	adev.ip_versions[GC_HWIP][0] = IP_VERSION(8, 0, 3);
	assert(rt_device_topology(&adev, &t) == 0 && t.xnack == RT_TARGET_FEATURE_UNSUPPORTED);

	/* XCCs summed over KFD nodes. */
	adev.ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	kfd.num_nodes = 2;
	kfd.nodes[1] = &node1;
	node0.xcc_mask = 0x3;
	node1.xcc_mask = 0xc;
	assert(rt_device_topology(&adev, &t) == 0 && t.xcc_count == 4);

	/* The device spec: upstream's GC config and active CUs and RBs, the
	 * KFD node's per-CU properties, GC 12's SA disable registers. */
	{
		static uint32_t gc_segments[2] = {0x1000, 0xa000};
		struct rt_device_spec spec;
		kfd.num_nodes = 1;
		node0.xcc_mask = 0x1;
		adev.ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
		adev.reg_offset[GC_HWIP][0] = gc_segments;
		assert(rt_device_spec(&adev, &spec) == -ENODEV && reg_reads == 0); /* GC not resolved */
		adev.gfx.config.max_shader_engines = 4;
		adev.gfx.config.max_sh_per_se = 2;
		adev.gfx.config.max_backends_per_se = 4;
		adev.gfx.config.max_cu_per_sh = 8;
		adev.gfx.config.backend_enable_mask = 0xfff;
		adev.gfx.config.num_rbs = 12;
		adev.gfx.cu_info.wave_front_size = 32;
		adev.gfx.cu_info.max_waves_per_simd = 16;
		adev.gfx.cu_info.number = 56;
		for (unsigned se = 0; se < 4; ++se)
			for (unsigned sa = 0; sa < 2; ++sa)
				adev.gfx.cu_info.bitmap[0][se][sa] = se == 3 && sa == 1 ? 0 : 0xff;
		assert(rt_device_spec(&adev, &spec) == 0);
		assert(spec.present == (RT_DEVICE_SPEC_GEOMETRY | RT_DEVICE_SPEC_CUS |
		       RT_DEVICE_SPEC_SHADER_ARRAYS | RT_DEVICE_SPEC_SA_DISABLE | RT_DEVICE_SPEC_BACKENDS));
		assert(spec.shader_engines == 4 && spec.shader_arrays_per_se == 2 &&
		       spec.backends_per_se == 4 && spec.cus_per_array == 8 && spec.wavefront_size == 32 &&
		       spec.max_waves_per_simd == 16 && spec.scratch_slots_per_cu == 32 &&
		       spec.lds_bytes == 65536 && spec.active_cus == 56);
		assert(spec.cu_bitmap[0][0] == 0xff && spec.cu_bitmap[3][1] == 0 &&
		       spec.active_sa_bitmap == 0x7f);
		assert(spec.cc_sa_disable == 0x00030000 && spec.user_sa_disable == 0x00040000 && reg_reads == 2);
		assert(spec.active_rb_bitmap == 0xfff && spec.active_rbs == 12);
		/* No register read on another GC, or with hardware access off. */
		adev.ip_versions[GC_HWIP][0] = IP_VERSION(11, 0, 0);
		assert(rt_device_spec(&adev, &spec) == 0 && !(spec.present & RT_DEVICE_SPEC_SA_DISABLE) &&
		       !spec.cc_sa_disable && reg_reads == 2);
		adev.ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
		adev.no_hw_access = true;
		assert(rt_device_spec(&adev, &spec) == 0 && !(spec.present & RT_DEVICE_SPEC_SA_DISABLE) &&
		       reg_reads == 2);
	}

	puts("device topology: KFD node/cache properties, XNACK mode, FRU name and device spec passed");
	return 0;
}
