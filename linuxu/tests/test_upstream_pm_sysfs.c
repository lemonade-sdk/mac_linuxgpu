/* The unmodified upstream amdgpu power-management sysfs (amdgpu_pm.c,
 * amdgpu_dpm.c, the swsmu amd_pm_funcs in amdgpu_smu.c) and the TTM memory
 * attributes (amdgpu_vram_mgr.c, amdgpu_gtt_mgr.c), read through linuxu's
 * sysfs exactly as a Linux tool reads /sys/class/drm/card0/device/.
 *
 * The SMU firmware is replaced by a fixture pptable_funcs below the swsmu
 * layer: amdgpu_pm.c decides which files exist (is_visible, attr_update)
 * and formats every value. No ASIC power-play table is linked, so the MP1
 * IP version names no real SMU (smu_set_funcs refuses it after installing
 * the swsmu amd_pm_funcs, as the fixture wants). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SWSMU_CODE_LAYER_L2
#include "amdgpu.h"
#include "amdgpu_pm.h"
#include "amdgpu_reset.h"
#include "amdgpu_dpm.h"
#include "amdgpu_smu.h"
#include "smu_cmn.h"
#include "kgd_pp_interface.h"
#include <rt/sysfs.h>

extern const struct amdgpu_ip_block_version smu_v14_0_ip_block;
extern const struct attribute_group amdgpu_vram_mgr_attr_group;
extern const struct attribute_group amdgpu_gtt_mgr_attr_group;

static struct gpu_metrics_v1_3 metrics;
static int fx_read_sensor(struct smu_context *smu, enum amd_pp_sensors sensor,
			  void *data, uint32_t *size)
{
	(void)smu;
	uint32_t *value = data;
	switch (sensor) {
	case AMDGPU_PP_SENSOR_GPU_LOAD: *value = 37; break;
	case AMDGPU_PP_SENSOR_MEM_LOAD: *value = 12; break;
	case AMDGPU_PP_SENSOR_EDGE_TEMP: *value = 45000; break;
	case AMDGPU_PP_SENSOR_HOTSPOT_TEMP: *value = 61000; break;
	case AMDGPU_PP_SENSOR_MEM_TEMP: *value = 58000; break;
	case AMDGPU_PP_SENSOR_GPU_AVG_POWER: *value = 123 << 8 | 50; break;	/* 123.050 W */
	case AMDGPU_PP_SENSOR_GFX_SCLK: *value = 245000; break;		/* 10 kHz units */
	case AMDGPU_PP_SENSOR_GFX_MCLK: *value = 125800; break;
	case AMDGPU_PP_SENSOR_VDDGFX: *value = 850; break;			/* mV */
	default: return -EOPNOTSUPP;
	}
	*size = 4;
	return 0;
}
static ssize_t fx_get_gpu_metrics(struct smu_context *smu, void **table)
{
	(void)smu;
	smu_cmn_init_soft_gpu_metrics(&metrics, 1, 3);
	metrics.temperature_edge = 45;
	metrics.average_gfx_activity = 37;
	metrics.average_umc_activity = 12;
	metrics.average_socket_power = 123;
	metrics.current_gfxclk = 2450;
	metrics.current_uclk = 1258;
	metrics.throttle_status = 0x10;
	*table = &metrics;
	return sizeof(metrics);
}
static int fx_emit_clk_levels(struct smu_context *smu, enum smu_clk_type type, char *buf, int *offset)
{
	(void)smu;
	if (type != SMU_SCLK && type != SMU_GFXCLK) return -EOPNOTSUPP;
	*offset += sysfs_emit_at(buf, *offset, "0: 500Mhz \n1: 2450Mhz *\n");
	return 0;
}
static int fx_get_fan_speed_rpm(struct smu_context *smu, uint32_t *speed)
{
	(void)smu;
	*speed = 1500;
	return 0;
}
static const struct pptable_funcs fixture_ppt = {
	.read_sensor = fx_read_sensor,
	.get_gpu_metrics = fx_get_gpu_metrics,
	.emit_clk_levels = fx_emit_clk_levels,
	.get_fan_speed_rpm = fx_get_fan_speed_rpm,
};
static const struct amdgpu_asic_funcs fixture_asic;

static struct amdgpu_device *adev;
static struct device dev;
static void release(struct device *d) { (void)d; }

static const char *rd(const char *path)
{
	static char buf[PAGE_SIZE];
	memset(buf, 0, sizeof(buf));
	long n = linuxu_sysfs_read(&dev.kobj, path, buf, sizeof(buf) - 1, 0, NULL);
	if (n < 0) {
		snprintf(buf, sizeof(buf), "errno %ld", -n);
	}
	return buf;
}
static void expect(const char *path, const char *text)
{
	const char *got = rd(path);
	if (strcmp(got, text)) {
		fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", path, got, text);
		abort();
	}
}

int main(void)
{
	adev = calloc(1, sizeof(*adev));
	assert(adev);
	device_initialize(&dev);
	dev.release = release;
	assert(!dev_set_name(&dev, "0000:03:00.0") && !device_add(&dev));
	adev->dev = &dev;
	dev_set_drvdata(&dev, adev_to_drm(adev));
	adev->asic_funcs = &fixture_asic;
	adev->ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	adev->ip_versions[MP1_HWIP][0] = IP_VERSION(99, 0, 0);
	amdgpu_set_init_level(adev, AMDGPU_INIT_LEVEL_DEFAULT);
	/* amdgpu_device_init's PM state. */
	mutex_init(&adev->pm.mutex);
	INIT_LIST_HEAD(&adev->pm.od_kobj_list);
	adev->pm.dpm_enabled = 1;
	static struct amdgpu_reset_domain domain;	/* not in reset */
	init_rwsem(&domain.sem);
	adev->reset_domain = &domain;

	/* smu_early_init installs the swsmu amd_pm_funcs and its context, then
	 * refuses the unknown MP1; the fixture supplies the pptable. */
	struct amdgpu_ip_block block = { .adev = adev, .version = &smu_v14_0_ip_block };
	assert(block.version->funcs->early_init(&block) == -EINVAL);
	struct smu_context *smu = adev->powerplay.pp_handle;
	assert(smu && adev->powerplay.pp_funcs);
	smu->ppt_funcs = &fixture_ppt;
	static char dpm_context[64];	/* smu_sw_init's DPM context */
	smu->smu_dpm.dpm_context = dpm_context;
	smu->smu_dpm.dpm_level = AMD_DPM_FORCED_LEVEL_AUTO;

	/* The PCI driver's dev_groups: TTM VRAM and GTT managers. */
	spin_lock_init(&adev->mman.bdev.lru_lock);
	adev->gmc.real_vram_size = 32ULL << 30;
	adev->gmc.visible_vram_size = 32ULL << 30;
	adev->mman.vram_mgr.manager.bdev = &adev->mman.bdev;
	adev->mman.vram_mgr.manager.usage = 3ULL << 30;
	adev->mman.gtt_mgr.manager.bdev = &adev->mman.bdev;
	adev->mman.gtt_mgr.manager.size = 16ULL << 30;
	adev->mman.gtt_mgr.manager.usage = 1ULL << 20;
	ttm_set_driver_manager(&adev->mman.bdev, TTM_PL_TT, &adev->mman.gtt_mgr.manager);
	/* Probe has initialized both managers before really_probe adds the
	 * groups; amdgpu_vram_attrs_is_visible hides them otherwise. */
	for (int i = 0; i < TTM_MAX_BO_PRIORITY; ++i) {
		INIT_LIST_HEAD(&adev->mman.vram_mgr.manager.lru[i]);
		INIT_LIST_HEAD(&adev->mman.gtt_mgr.manager.lru[i]);
	}
	ttm_resource_manager_set_used(&adev->mman.vram_mgr.manager, true);
	ttm_resource_manager_set_used(&adev->mman.gtt_mgr.manager, true);
	const struct attribute_group *dev_groups[] = {
		&amdgpu_vram_mgr_attr_group, &amdgpu_gtt_mgr_attr_group, NULL,
	};
	assert(!device_add_groups(&dev, dev_groups));

	assert(!amdgpu_pm_sysfs_init(adev));
	expect("gpu_busy_percent", "37\n");
	expect("mem_busy_percent", "12\n");
	expect("pp_dpm_sclk", "0: 500Mhz \n1: 2450Mhz *\n");
	expect("power_dpm_force_performance_level", "auto\n");
	expect("mem_info_vram_total", "34359738368\n");
	expect("mem_info_vram_used", "3221225472\n");
	expect("mem_info_gtt_total", "17179869184\n");
	expect("mem_info_gtt_used", "1048576\n");
	/* hwmon, found the way Linux tools scan device/hwmon/. */
	char list[256] = {0};
	assert(linuxu_sysfs_list(&dev.kobj, "hwmon", list, sizeof(list) - 1, 0, NULL) > 0);
	assert(!strcmp(list, "d hwmon0\n"));
	expect("hwmon/hwmon0/name", "amdgpu\n");
	expect("hwmon/hwmon0/temp1_input", "45000\n");
	expect("hwmon/hwmon0/temp1_label", "edge\n");
	expect("hwmon/hwmon0/temp2_input", "61000\n");
	expect("hwmon/hwmon0/temp2_label", "junction\n");
	expect("hwmon/hwmon0/temp3_input", "58000\n");
	expect("hwmon/hwmon0/temp3_label", "mem\n");
	expect("hwmon/hwmon0/power1_average", "123050000\n");
	expect("hwmon/hwmon0/freq1_input", "2450000000\n");
	expect("hwmon/hwmon0/freq1_label", "sclk\n");
	expect("hwmon/hwmon0/in0_input", "850\n");
	expect("hwmon/hwmon0/in0_label", "vddgfx\n");
	expect("hwmon/hwmon0/fan1_input", "1500\n");

	/* gpu_metrics: the SMU's table, header first. */
	unsigned char raw[PAGE_SIZE];
	size_t length = 0;
	long n = linuxu_sysfs_read(&dev.kobj, "gpu_metrics", raw, sizeof(raw), 0, &length);
	assert(n == (long)sizeof(metrics) && length == sizeof(metrics));
	struct gpu_metrics_v1_3 got;
	memcpy(&got, raw, sizeof(got));
	assert(got.common_header.structure_size == sizeof(metrics) &&
	       got.common_header.format_revision == 1 && got.common_header.content_revision == 3);
	assert(got.average_gfx_activity == 37 && got.current_gfxclk == 2450 &&
	       got.throttle_status == 0x10);

	/* A sensor the SMU lacks is the show()'s errno, and an attribute the
	 * device lacks is no file at all. */
	expect("hwmon/hwmon0/power1_input", "errno 2");
	expect("pp_dpm_mclk", "errno 95");
	expect("vcn_busy_percent", "errno 95");

	amdgpu_pm_sysfs_fini(adev);
	expect("gpu_busy_percent", "errno 2");
	expect("hwmon/hwmon0/name", "errno 2");
	device_remove_groups(&dev, dev_groups);
	expect("mem_info_vram_total", "errno 2");
	kfree(smu);
	device_unregister(&dev);
	assert(!linuxu_sysfs_count(NULL));
	free(adev);
	puts("PASS upstream amdgpu_pm/vram/gtt sysfs through linuxu: device attrs, hwmon, gpu_metrics, errnos, fini");
	return 0;
}
