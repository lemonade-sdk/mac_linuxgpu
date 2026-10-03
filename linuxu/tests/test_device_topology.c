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

	puts("device topology: KFD node/cache properties, XNACK mode and FRU name passed");
	return 0;
}
