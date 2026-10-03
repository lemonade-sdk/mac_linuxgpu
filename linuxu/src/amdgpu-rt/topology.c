/* Device description for the HSA runtime, read from upstream KFD topology.
 *
 * kgd2kfd_device_init() adds every KFD node to the topology
 * (kfd_topology_add_device()), which fills the node properties ROCr reads
 * from sysfs on Linux: gfx_target_version, SIMD/wave/LDS/scratch geometry,
 * engine clock, capabilities, and the cache properties
 * (kfd_fill_cache_non_crat_info()).  This file copies those values; it
 * derives none of them. */
#include <linux/errno.h>
#include <linux/kfd_sysfs.h>
#include <rt/compute.h>
#include "amdgpu.h"
#include "amdgpu_fru_eeprom.h"
#include "kfd_priv.h"
#include "kfd_topology.h"

_Static_assert(sizeof(((struct rt_compute_topology *)0)->product_name) ==
	       AMDGPU_PRODUCT_NAME_LEN, "product name length");

/* KFD's own XNACK selection for a process using this node alone
 * (kfd_process_xnack_mode(p, false), which kfd_create_process() uses for
 * the default mode).  It reads only the process's device list. */
static uint32_t xnack_mode(struct kfd_node *node)
{
	struct kfd_process *p;
	struct kfd_process_device *pdd;
	uint32_t mode;

	if (!KFD_IS_SOC15(node))
		return RT_TARGET_FEATURE_UNSUPPORTED;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	pdd = kzalloc(sizeof(*pdd), GFP_KERNEL);
	if (!p || !pdd) {
		kfree(p);
		kfree(pdd);
		return RT_TARGET_FEATURE_UNSUPPORTED;
	}
	pdd->dev = node;
	p->pdds[0] = pdd;
	p->n_pdds = 1;
	mode = kfd_process_xnack_mode(p, false) ? RT_TARGET_FEATURE_ON :
						 RT_TARGET_FEATURE_OFF;
	kfree(pdd);
	kfree(p);
	return mode;
}

/* First data cache KFD reports at this level (bytes; 0 without one). */
static uint64_t cache_bytes(struct kfd_topology_device *dev, uint32_t level)
{
	struct kfd_cache_properties *cache;

	list_for_each_entry(cache, &dev->cache_props, list)
		if (cache->cache_level == level &&
		    (cache->cache_type & HSA_CACHE_TYPE_DATA))
			return (uint64_t)cache->cache_size * 1024;
	return 0;
}

int rt_device_topology(struct amdgpu_device *adev, struct rt_compute_topology *out)
{
	struct kfd_dev *kfd;
	struct kfd_topology_device *dev;
	const struct kfd_node_properties *props;

	if (!adev || !out)
		return -EINVAL;
	kfd = adev->kfd.dev;
	if (!kfd || !kfd->init_complete || !kfd->num_nodes || !kfd->nodes[0])
		return -ENODEV;
	dev = kfd_topology_device_by_id(kfd->nodes[0]->id);
	if (!dev || dev->gpu != kfd->nodes[0])
		return -ENODEV;
	props = &dev->node_props;

	memset(out, 0, sizeof(*out));
	out->gfx_target_version = props->gfx_target_version;
	out->simd_per_cu = props->simd_per_cu;
	out->max_waves_per_simd = props->max_waves_per_simd;
	out->lds_bytes = props->lds_size_in_kb * 1024;
	out->scratch_slots_per_cu = props->max_slots_scratch_cu;
	out->l1_bytes = cache_bytes(dev, 1);
	out->l2_bytes = cache_bytes(dev, 2);
	out->l3_bytes = cache_bytes(dev, 3);
	out->max_engine_clock_mhz = props->max_engine_clk_fcompute;
	out->xnack = xnack_mode(kfd->nodes[0]);
	/* ROCr: sramecc is on exactly when KFD reports SRAM EDC support. */
	out->sramecc = (props->capability & HSA_CAP_SRAM_EDCSUPPORTED) ?
		       RT_TARGET_FEATURE_ON : RT_TARGET_FEATURE_OFF;
	for (unsigned i = 0; i < kfd->num_nodes; ++i)
		if (kfd->nodes[i])
			out->xcc_count += NUM_XCC(kfd->nodes[i]->xcc_mask);
	/* The board's FRU product name, as amdgpu's product_name sysfs file
	 * reports it; boards without a FRU EEPROM have none. */
	if (adev->fru_info)
		memcpy(out->product_name, adev->fru_info->product_name,
		       sizeof(out->product_name));
	return 0;
}
