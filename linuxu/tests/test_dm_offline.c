/* Offline Display Core probe: the unmodified amdgpu_dm IP block (dm_ip_block)
 * and Display Core run against a fixture DCN 4.0.1 device, linked from the
 * host library like the other upstream tests (make lib).
 *
 * The fixture is a device with no GPU behind it:
 *   - a synthetic atomfirmware VBIOS (ATOM ROM header v2.2, master data
 *     table v2.1, firmware info v3.4, display controller info v4.5, SMU
 *     info v4.0, display object info v1.5 with three DisplayPort and one
 *     HDMI path, their DDC and HPD records and a GPIO pin LUT). It
 *     describes a board; it is not any vendor's VBIOS.
 *   - a 64 MiB register file behind amdgpu's indirect register hooks
 *     (adev->reg.pcie, no direct MMIO aperture): every register reads
 *     back what was last written and nothing responds by itself. Only the
 *     CC_DC_PIPE_DIS fuse is preset (DMCUB present).
 *   - 64 MiB of VRAM in host memory under the real amdgpu_ttm_init() and
 *     VRAM manager (GMC v12 callbacks from gmc_v12_0 early_init).
 *   - the device interrupt from the real amdgpu_irq_init().
 *   - the real linux-firmware DCN 4.0.1 DMCUB image, served by the linuxu
 *     firmware loader from the embedded table (firmware/firmware.lock).
 *
 *   - a client file opened like amdgpu_driver_open_kms opens one (its VM
 *     with CPU page-table updates and a scheduler no job reaches), so
 *     dumb buffers and framebuffers are the real amdgpu ones.
 *
 * It runs dm_early_init, dm_sw_init, dm_hw_init, dm_hw_fini and
 * dm_sw_fini as amdgpu_device_ip_init()/ip_fini() would. Between hw_init
 * and hw_fini the DRM device is registered as amdgpu_pci_probe registers
 * it, two connectors get a sink (forced on with a synthetic EDID as
 * override, the Linux way to emulate a monitor) and the display test
 * client (rt/display.h) probes, shows patterns on one output and on all,
 * and turns them off, checked through connector sysfs, the framebuffer
 * contents in VRAM and the OTG/HUBP registers the commit programmed. Everything up to
 * a running display microcontroller is exercised: the DMCUB image is
 * placed in its framebuffer and the controller is reset and released,
 * but no DMCUB executes it, so the auto-load wait and the DMUB commands
 * DC sends afterwards time out, as do the power-gate status waits. No
 * sink is attached, so every connector is undetected.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amdgpu.h"
#include "atomfirmware.h"
#include "atom.h"
#include "amdgpu_dm.h"
#include "amdgpu_atombios.h"
#include "dmub/dmub_srv.h"
#include <linux/firmware.h>
#include <drm/drm_connector.h>
#include "ObjectID.h"
#include "dc.h"
#include "dcn/dcn_4_1_0_offset.h"
#include "dcn/dcn_4_1_0_sh_mask.h"
#include "dcn401_fixture.h"
#include <rt/bootstrap.h>
#include <drm/drm_drv.h>
#include "amdgpu_ttm.h"
#include "amdgpu_reset.h"
#include "amdgpu_vm.h"
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_file.h>
#include <drm/gpu_scheduler.h>
#include <rt/display.h>
#include <rt/identity.h>
#include <rt/surface.h>
#include <rt/removal.h>
#include <rt/sysfs.h>

extern const struct amdgpu_ip_block_version dm_ip_block;
extern const struct amdgpu_ip_block_version gmc_v12_0_ip_block;
extern int linuxu_workqueue_init(void);
extern int linuxu_timer_service_init(void);
extern int linuxu_sysinfo_init(void);
extern int fw_table_register_embedded(void);
extern int linuxu_module_init_drm_core_init(void);
extern int linuxu_module_init_gpu_buddy_module_init(void);
extern int linuxu_module_init_drm_sched_fence_slab_init(void);
extern int amdgpu_sync_init(void);
/* drm_crtc_internal.h: what a write to debugfs edid_override runs. */
extern int drm_edid_override_set(struct drm_connector *connector, const void *edid, size_t size);

static struct amdgpu_device *adev;

/* The PCI function, whose device is the DRM device's parent (adev->dev),
 * as amdgpu_pci_probe sets it up. */
static struct pci_dev fixture_pdev;
#define fixture_dev fixture_pdev.dev
static struct amdgpu_reset_domain fixture_reset_domain;
/* A client file the way amdgpu_driver_open_kms makes one, minus what the
 * fixture has no engine or GART for (seq64, user queues): the file's VM,
 * its root page directory in fixture VRAM and updated by the CPU, the
 * eviction-fence manager and the context manager. It closes through the
 * real amdgpu_driver_postclose_kms. */
static int fixture_open(struct drm_device *dev, struct drm_file *file)
{
	struct amdgpu_device *a = drm_to_adev(dev);
	struct amdgpu_fpriv *fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	struct drm_exec exec;
	int r;

	if (!fpriv)
		return -ENOMEM;
	r = amdgpu_vm_init(a, &fpriv->vm, 0, 0);
	if (r) {
		kfree(fpriv);
		return r;
	}
	drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES, 0);
	drm_exec_until_all_locked(&exec) {
		r = amdgpu_vm_lock_pd(&fpriv->vm, &exec, 0);
		drm_exec_retry_on_contention(&exec);
		if (r)
			break;
	}
	if (!r)
		fpriv->prt_va = amdgpu_vm_bo_add(a, &fpriv->vm, NULL);
	drm_exec_fini(&exec);
	assert(!r && fpriv->prt_va);
	mutex_init(&fpriv->bo_list_lock);
	idr_init_base(&fpriv->bo_list_handles, 1);
	amdgpu_evf_mgr_init(&fpriv->evf_mgr);
	amdgpu_ctx_mgr_init(&fpriv->ctx_mgr, a);
	file->driver_priv = fpriv;
	return 0;
}

static unsigned long fixture_files_closed;

static void fixture_postclose(struct drm_device *dev, struct drm_file *file)
{
	amdgpu_driver_postclose_kms(dev, file);
	fixture_files_closed++;
}

static const struct drm_driver fixture_driver = {
	.name = "amdgpu",
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER,
	.open = fixture_open,
	.postclose = fixture_postclose,
	.dumb_create = amdgpu_mode_dumb_create,
};

/* The VM's page-table entities need a scheduler; with CPU page-table
 * updates no job ever reaches it. */
static struct dma_fence *fixture_run_job(struct drm_sched_job *job)
{
	(void)job;
	assert(!"no VM job expected with CPU page-table updates");
	return NULL;
}

static enum drm_gpu_sched_stat fixture_timedout_job(struct drm_sched_job *job)
{
	(void)job;
	return DRM_GPU_SCHED_STAT_RESET;
}

static void fixture_free_job(struct drm_sched_job *job)
{
	(void)job;
}

static const struct drm_sched_backend_ops fixture_sched_ops = {
	.run_job = fixture_run_job,
	.timedout_job = fixture_timedout_job,
	.free_job = fixture_free_job,
};
static struct drm_gpu_scheduler fixture_sched;

/* Fixture VRAM: TTM's VRAM heap over host memory. amdgpu_ttm_init() is the
 * real one; the CPU view of VRAM (aper_base_kaddr, an ioremap_wc() of the
 * BAR on hardware) is this buffer. */
#define VRAM_BYTES	(64ULL << 20)
static void *fixture_vram;

static void fixture_ttm_init(void)
{
	struct amdgpu_ip_block gmc = { .adev = adev, .version = &gmc_v12_0_ip_block };

	/* The real GMC v12 callbacks (VBIOS framebuffer size and friends). */
	assert(gmc.version->funcs->early_init(&gmc) == 0);
	adev->gmc.real_vram_size = VRAM_BYTES;
	adev->gmc.visible_vram_size = VRAM_BYTES;
	adev->gmc.mc_vram_size = VRAM_BYTES;
	adev->gmc.vram_start = 0x8000000000ULL;
	adev->gmc.vram_end = adev->gmc.vram_start + VRAM_BYTES - 1;
	adev->gmc.aper_base = 0xd0000000ULL;
	adev->gmc.aper_size = VRAM_BYTES;
	adev->gmc.gart_size = 512ULL << 20;
	adev->gmc.gart_start = 0x0ULL;
	adev->gmc.gart_end = adev->gmc.gart_size - 1;
	fixture_vram = calloc(1, VRAM_BYTES);
	assert(fixture_vram);
	/* gmc_v12_0_sw_init's VM sizes, then TTM and the VM manager. */
	amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48);
	assert(amdgpu_ttm_init(adev) == 0);
	adev->mman.aper_base_kaddr = fixture_vram;
	adev->vm_manager.vram_base_offset = adev->gmc.vram_start;
	amdgpu_vm_manager_init(adev);
	/* The fixture has no SDMA engine: page tables are written by the CPU
	 * (amdgpu.vm_update_mode=3, which arm64 builds otherwise ignore). */
	adev->vm_manager.vm_update_mode = AMDGPU_VM_USE_CPU_FOR_GFX | AMDGPU_VM_USE_CPU_FOR_COMPUTE;
	{
		struct drm_sched_init_args args = {
			.ops = &fixture_sched_ops,
			.num_rqs = DRM_SCHED_PRIORITY_COUNT,
			.credit_limit = 1,
			.timeout = MAX_SCHEDULE_TIMEOUT,
			.name = "fixture-vm",
			.dev = adev->dev,
		};

		assert(drm_sched_init(&fixture_sched, &args) == 0);
		adev->vm_manager.vm_pte_scheds[0] = &fixture_sched;
		adev->vm_manager.vm_pte_num_scheds = 1;
	}
}

static void fixture_noop_work(struct work_struct *work)
{
	(void)work;
}

static void fixture_init(void)
{
	device_initialize(&fixture_dev);
	assert(dev_set_name(&fixture_dev, "0000:03:00.0") >= 0);
	/* DRM publishes its minors under a registered parent. */
	assert(device_add(&fixture_dev) == 0);
	fixture_pdev.vendor = 0x1002;
	fixture_pdev.device = 0x7551;
	fixture_pdev.revision = 0xc0;
	/* The DRM device the way amdgpu_pci_probe() allocates it. */
	adev = devm_drm_dev_alloc(&fixture_dev, &fixture_driver,
				  struct amdgpu_device, ddev);
	assert(!IS_ERR_OR_NULL(adev));
	adev->dev = &fixture_dev;
	adev->pdev = &fixture_pdev;
	/* amdgpu_device_init()'s locks and reset domain. */
	init_rwsem(&fixture_reset_domain.sem);
	adev->reset_domain = &fixture_reset_domain;
	adev->usec_timeout = AMDGPU_MAX_USEC_TIMEOUT;
	RCU_INIT_POINTER(adev->gang_submit, dma_fence_get_stub());
	adev->fence_context = dma_fence_context_alloc(AMDGPU_MAX_RINGS);
	mutex_init(&adev->notifier_lock);
	INIT_DELAYED_WORK(&adev->delayed_init_work, fixture_noop_work);
	/* The display test finds the DRM device as the PCI driver data. */
	dev_set_drvdata(&fixture_pdev.dev, adev_to_drm(adev));
	mutex_init(&adev->firmware.mutex);
	mutex_init(&adev->pm.mutex);
	mutex_init(&adev->grbm_idx_mutex);
	mutex_init(&adev->srbm_mutex);
	mutex_init(&adev->mn_lock);
	dev_set_drvdata(&fixture_dev, adev_to_drm(adev));
	/* amdgpu_device_init() initializes mode config before IP init. */
	drm_mode_config_init(adev_to_drm(adev));

	adev->asic_type = CHIP_IP_DISCOVERY;
	adev->family = AMDGPU_FAMILY_GC_12_0_0;
	adev->external_rev_id = 0x50;
	adev->ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	adev->ip_versions[MMHUB_HWIP][0] = IP_VERSION(4, 1, 0);
	adev->firmware.load_type = AMDGPU_FW_LOAD_PSP;
	amdgpu_set_init_level(adev, AMDGPU_INIT_LEVEL_DEFAULT);

	/* DCN 4.0.1: bases, the register file, the DMCUB fuse. */
	dcn401_fixture_attach(adev);
	fixture_ttm_init();
	/* The IH block's sw_init: the device interrupt and its sources. */
	printf("dm-offline: amdgpu_irq_init -> %d\n", amdgpu_irq_init(adev));

	dcn401_fixture_vbios(adev);
}


/* ---- the display test ---- */

static struct drm_connector *connector_named(const char *name)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector, *found = NULL;

	drm_connector_list_iter_begin(adev_to_drm(adev), &iter);
	drm_for_each_connector_iter(connector, &iter)
		if (!strcmp(connector->name, name))
			found = connector;
	drm_connector_list_iter_end(&iter);
	return found;
}

/* A file under the PCI device's sysfs directory, as Linux shows
 * /sys/bus/pci/devices/<bdf>/drm/card0/... */
static long sysfs_text(const char *path, char *buf, size_t size)
{
	size_t length = 0;
	long r = linuxu_sysfs_read(&fixture_dev.kobj, path, buf, size - 1, 0, &length);

	buf[r > 0 ? r : 0] = '\0';
	printf("dm-offline: sysfs %s -> %ld %s%s", path, r, r > 0 ? buf : "",
	       r > 0 && buf[r - 1] == '\n' ? "" : "\n");
	return r;
}

static const struct rt_display_connector *report_connector(const struct rt_display_report *report,
							   const char *name)
{
	for (uint32_t i = 0; i < report->connectors; i++)
		if (!strcmp(report->connector[i].name, name))
			return &report->connector[i];
	return NULL;
}

/* The DCN 4.0.1 pipes the commit programmed: an OTG with 1080p60 totals
 * and a HUBP scanning out of the framebuffer. */
#define OTG_STRIDE	(regOTG1_OTG_H_TOTAL - regOTG0_OTG_H_TOTAL)
#define HUBP_STRIDE	(regHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS - regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS)
static int otg_with_timing(uint32_t htotal, uint32_t vtotal)
{
	for (int i = 0; i < 4; i++) {
		uint32_t h = RREG32(dcn401_dce_base[regOTG0_OTG_H_TOTAL_BASE_IDX] + regOTG0_OTG_H_TOTAL + i * OTG_STRIDE);
		uint32_t v = RREG32(dcn401_dce_base[regOTG0_OTG_V_TOTAL_BASE_IDX] + regOTG0_OTG_V_TOTAL + i * OTG_STRIDE);

		if ((h & 0x7fff) == htotal - 1 && (v & 0xffff) == vtotal - 1)
			return i;
	}
	return -1;
}

static int hubp_scanning(uint64_t address)
{
	for (int i = 0; i < 4; i++)
		if (RREG32(dcn401_dce_base[regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX] +
			   regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS + i * HUBP_STRIDE) ==
		    lower_32_bits(address))
			return i;
	return -1;
}

static uint32_t fb_pixel(const struct rt_display_report *report, uint32_t x, uint32_t y)
{
	const uint8_t *fb = (const uint8_t *)fixture_vram + (report->fb_gpu_addr - adev->gmc.vram_start);

	return *(const uint32_t *)(fb + (size_t)y * report->fb_pitch + x * 4) & 0xffffff;
}

static void display_test(void)
{
	struct drm_device *ddev = adev_to_drm(adev);
	struct drm_connector *hdmi = connector_named("HDMI-A-1");
	struct drm_connector *dp = connector_named("DP-2");
	struct rt_display_report report;
	const struct rt_display_connector *c;
	unsigned long writes;
	char text[4096];
	int otg, hubp, r;

	/* Sinks on HDMI-A-1 and DP-2: what video=<connector>:e and a write of
	 * the EDID to debugfs edid_override do on Linux (DM builds an emulated
	 * sink from it), since no fixture DDC or AUX channel answers. DP-1 and
	 * DP-3 stay empty. */
	assert(hdmi && dp);
	dcn401_fixture_build_edid();
	hdmi->force = DRM_FORCE_ON;
	assert(drm_edid_override_set(hdmi, dcn401_fixture_edid, sizeof(dcn401_fixture_edid)) == 0);
	dp->force = DRM_FORCE_ON;
	assert(drm_edid_override_set(dp, dcn401_fixture_edid_dp, sizeof(dcn401_fixture_edid_dp)) == 0);

	/* amdgpu_pci_probe registers the device after IP init: minors, then
	 * planes, CRTCs, encoders and connectors with their sysfs devices. */
	r = drm_dev_register(ddev, 0);
	printf("dm-offline: drm_dev_register -> %d\n", r);
	assert(r == 0);
	/* Not probed yet: what Linux shows before any client probes. */
	assert(sysfs_text("drm/card0/card0-DP-1/status", text, sizeof(text)) > 0);
	assert(!strcmp(text, "unknown\n"));
	assert(linuxu_sysfs_list(&fixture_dev.kobj, "drm/card0", text, sizeof(text) - 1, 0, NULL) > 0);
	assert(strstr(text, "d card0-HDMI-A-1\n") && strstr(text, "d card0-DP-3\n"));
	assert(strstr(text, "f dev\n") || strstr(text, "d card0-DP-1\n"));

	/* Probe: every connector's fill_modes, as the client would. */
	r = rt_display_probe(&fixture_pdev, &report);
	printf("dm-offline: display probe -> %d (probe %d), %u connector(s)\n", r,
	       report.probe_status, report.connectors);
	assert(r == 0 && report.version == RT_DISPLAY_VERSION && report.crtcs == 4);
	for (uint32_t i = 0; i < report.connectors; i++)
		printf("dm-offline:   %-9s status %u, %u mode(s), EDID %u bytes, preferred %ux%u@%u\n",
		       report.connector[i].name, report.connector[i].status, report.connector[i].modes,
		       report.connector[i].edid_bytes, report.connector[i].preferred_width,
		       report.connector[i].preferred_height, report.connector[i].preferred_refresh);
	c = report_connector(&report, "HDMI-A-1");
	assert(c && c->status == connector_status_connected && c->modes > 0 &&
	       c->edid_bytes == sizeof(dcn401_fixture_edid) && !c->lit);
	assert(c->preferred_width == 1920 && c->preferred_height == 1080 && c->preferred_refresh == 60);
	c = report_connector(&report, "DP-2");
	assert(c && c->status == connector_status_connected && c->modes > 0 &&
	       c->edid_bytes == sizeof(dcn401_fixture_edid_dp) && c->preferred_width == 1920 &&
	       c->preferred_height == 1080 && c->preferred_refresh == 60);
	c = report_connector(&report, "DP-1");
	assert(c && c->status == connector_status_disconnected && !c->modes && !c->edid_bytes);
	assert(!report.showing);
	assert(sysfs_text("drm/card0/card0-DP-1/status", text, sizeof(text)) > 0);
	assert(!strcmp(text, "disconnected\n"));

	/* A display agent's view: cached status and the hotplug epoch (the
	 * first call registers the monitor client: epoch 1), a hotplug event
	 * as DM's HPD handler sends it, and one connector's modes. */
	{
		struct rt_display_modes modes;
		uint32_t epoch;

		assert(rt_display_status(&fixture_pdev, &report) == 0);
		epoch = report.hotplug_epoch;
		assert(epoch >= 1 && report_connector(&report, "DP-2")->status == connector_status_connected);
		drm_kms_helper_hotplug_event(ddev);
		assert(rt_display_status(&fixture_pdev, &report) == 0);
		printf("dm-offline: display status: hotplug epoch %u -> %u\n", epoch, report.hotplug_epoch);
		assert(report.hotplug_epoch == epoch + 1);
		assert(rt_display_modes(&fixture_pdev, "HDMI-A-1", &modes) == 0);
		printf("dm-offline: HDMI-A-1 modes: %u of %u, %ux%u mm, first %ux%u@%u.%03u flags %#x\n",
		       modes.count, modes.total, modes.width_mm, modes.height_mm, modes.mode[0].width,
		       modes.mode[0].height, modes.mode[0].refresh_mhz / 1000,
		       modes.mode[0].refresh_mhz % 1000, modes.mode[0].flags);
		assert(modes.status == connector_status_connected && modes.count > 1 &&
		       modes.count == modes.total && modes.width_mm == 600 && modes.height_mm == 340);
		assert(modes.mode[0].width == 1920 && modes.mode[0].height == 1080 &&
		       modes.mode[0].refresh_mhz == 60000 && modes.mode[0].clock_khz == 148500 &&
		       (modes.mode[0].flags & RT_DISPLAY_MODE_PREFERRED));
		assert(rt_display_modes(&fixture_pdev, "DP-1", &modes) == 0 && !modes.count &&
		       modes.status == connector_status_disconnected);
		assert(rt_display_modes(&fixture_pdev, "DP-9", &modes) == -ENOENT);
	}

	/* The monitor on a connector, as the dext publishes it for our tools:
	 * the EDID's monitor name descriptor and the physical size, from what
	 * the probe left (no detection). */
	{
		struct rt_display_monitor monitor;

		assert(rt_display_monitor(&fixture_pdev, "HDMI-A-1", &monitor) == 0);
		printf("dm-offline: HDMI-A-1 monitor \"%s\", %ux%u mm\n", monitor.name,
		       monitor.width_mm, monitor.height_mm);
		assert(monitor.status == connector_status_connected &&
		       !strcmp(monitor.name, "LINUXU TEST") && monitor.width_mm == 600 &&
		       monitor.height_mm == 340);
		assert(rt_display_monitor(&fixture_pdev, "card0-DP-2", &monitor) == 0 &&
		       !strcmp(monitor.name, "LINUXU DP"));
		assert(rt_display_monitor(&fixture_pdev, "DP-1", &monitor) == 0 &&
		       monitor.status == connector_status_disconnected && !monitor.name[0] &&
		       !monitor.width_mm);
		assert(rt_display_monitor(&fixture_pdev, "DP-9", &monitor) == -ENOENT);
		assert(rt_display_monitor(&fixture_pdev, "", &monitor) == -EINVAL);
		assert(rt_display_monitor(&fixture_pdev, "DP-2", NULL) == -EINVAL);
	}

	/* The board as the driver knows it (rt/identity.h): copies of the
	 * device's own fields. This fixture runs no KFD, so it has no ISA
	 * target. */
	{
		struct rt_device_identity id;
		struct atom_context *atom = adev->mode_info.atom_context;
		static struct pci_dev unbound;

		assert(rt_device_identity(&fixture_pdev, &id) == 0);
		printf("dm-offline: identity %04x:%04x rev %02x, GC %u.%u.%u, VRAM %llu MB %s, "
		       "VBIOS \"%s\" \"%s\"\n", id.vendor, id.device, id.revision,
		       id.gc_version >> 24, (id.gc_version >> 16) & 0xff, (id.gc_version >> 8) & 0xff,
		       (unsigned long long)(id.vram_bytes >> 20), id.vram_type_name, id.vbios_pn,
		       id.vbios_version);
		assert(id.version == RT_IDENTITY_VERSION && id.vendor == 0x1002 &&
		       id.device == 0x7551 && id.revision == 0xc0);
		assert(id.gc_version == IP_VERSION(12, 0, 1) && !id.gfx_target_version &&
		       !id.gfx_target[0]);
		assert(id.vram_bytes == adev->gmc.real_vram_size && id.vram_bytes &&
		       id.visible_vram_bytes == adev->gmc.visible_vram_size &&
		       id.vram_type == adev->gmc.vram_type &&
		       id.compute_units == adev->gfx.cu_info.number);
		assert(atom && !strncmp(id.vbios_pn, (const char *)atom->vbios_pn, strlen(id.vbios_pn)) &&
		       !strncmp(id.vbios_version, (const char *)atom->vbios_ver_str,
				strlen(id.vbios_version)));
		assert(!id.product_name[0]);
		assert(rt_device_identity(&unbound, &id) == -ENODEV && id.device == 0);
		assert(rt_device_identity(&fixture_pdev, NULL) == -EINVAL);
	}

	/* drm_sysfs connector files, read as a Linux tool reads them. */
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/status", text, sizeof(text)) > 0);
	assert(!strcmp(text, "connected\n"));
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/enabled", text, sizeof(text)) > 0);
	assert(!strcmp(text, "disabled\n"));
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/dpms", text, sizeof(text)) > 0);
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/modes", text, sizeof(text)) > 0);
	assert(!strncmp(text, "1920x1080\n", 10));
	{
		size_t length = 0;
		long n = linuxu_sysfs_read(&fixture_dev.kobj, "drm/card0/card0-HDMI-A-1/edid",
					   text, sizeof(text), 0, &length);

		printf("dm-offline: sysfs drm/card0/card0-HDMI-A-1/edid -> %ld bytes\n", n);
		assert(n == sizeof(dcn401_fixture_edid) && !memcmp(text, dcn401_fixture_edid, sizeof(dcn401_fixture_edid)));
		n = linuxu_sysfs_read(&fixture_dev.kobj, "drm/card0/card0-DP-1/edid",
				      text, sizeof(text), 0, &length);
		assert(n == 0);
	}

	/* Refusals leave nothing behind. */
	assert(rt_display_show(&fixture_pdev, "HDMI-A-1", RT_DISPLAY_PATTERNS, &report) == -EINVAL);
	r = rt_display_show(&fixture_pdev, "DP-1", RT_DISPLAY_PATTERN_BARS, &report);
	printf("dm-offline: display show DP-1 (no sink) -> %d\n", r);
	assert(r == -ENOENT && !report.showing && !rt_display_showing());
	assert(rt_display_show(&fixture_pdev, "DP-9", RT_DISPLAY_PATTERN_BARS, &report) == -ENOENT);

	/* The pattern on HDMI-A-1: dumb buffer in VRAM, pattern written
	 * through the CPU view of VRAM, atomic commit through amdgpu_dm/DC. */
	writes = dcn401_reg_writes;
	r = rt_display_show(&fixture_pdev, "HDMI-A-1", RT_DISPLAY_PATTERN_BARS, &report);
	printf("dm-offline: display show HDMI-A-1 -> %d (probe %d, commit %d), fb %ux%u pitch %u at 0x%llx, "
	       "fill %llu us, commit %llu ms, %lu register writes\n", r, report.probe_status,
	       report.commit_status, report.fb_width, report.fb_height, report.fb_pitch,
	       (unsigned long long)report.fb_gpu_addr, (unsigned long long)report.fill_ns / 1000,
	       (unsigned long long)report.commit_ns / 1000000, dcn401_reg_writes - writes);
	assert(r == 0 && report.showing && rt_display_showing());
	assert(report.pattern == RT_DISPLAY_PATTERN_BARS);
	assert(report.fb_width == 1920 && report.fb_height == 1080 && report.fb_pitch >= 1920 * 4);
	assert(report.fb_gpu_addr >= adev->gmc.vram_start &&
	       report.fb_gpu_addr + (uint64_t)report.fb_pitch * 1080 <= adev->gmc.vram_end + 1);
	c = report_connector(&report, "HDMI-A-1");
	assert(c && c->lit && c->lit_width == 1920 && c->lit_height == 1080 && c->lit_refresh == 60);
	/* The bars: white frame, then white, yellow, cyan, ... and the ramp. */
	assert(fb_pixel(&report, 0, 0) == 0xffffff);
	assert(fb_pixel(&report, 1920 / 16, 100) == 0xffffff);
	assert(fb_pixel(&report, 1920 * 3 / 16, 100) == 0xffff00);
	assert(fb_pixel(&report, 1920 * 13 / 16, 100) == 0x0000ff);
	assert(fb_pixel(&report, 1920 * 15 / 16, 100) == 0x000000);
	assert(fb_pixel(&report, 1919 - 4, 1000) == 0xfefefe || fb_pixel(&report, 1919 - 4, 1000) == 0xffffff);
	otg = otg_with_timing(2200, 1125);
	hubp = hubp_scanning(report.fb_gpu_addr);
	printf("dm-offline: OTG%d has 2200x1125 totals, HUBP%d scans out 0x%08x\n", otg, hubp,
	       lower_32_bits(report.fb_gpu_addr));
	assert(otg >= 0 && hubp >= 0);
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/enabled", text, sizeof(text)) > 0);
	assert(!strcmp(text, "enabled\n"));

	/* Off: the saved (all-off) state is committed again. */
	r = rt_display_off(&fixture_pdev, &report);
	printf("dm-offline: display off -> %d (restore %d, %llu ms)\n", r, report.restore_status,
	       (unsigned long long)report.commit_ns / 1000000);
	assert(r == 0 && !report.showing && !rt_display_showing());
	c = report_connector(&report, "HDMI-A-1");
	assert(c && !c->lit);
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/enabled", text, sizeof(text)) > 0);
	assert(!strcmp(text, "disabled\n"));
	{
		uint32_t control = RREG32(dcn401_dce_base[regOTG0_OTG_CONTROL_BASE_IDX] + regOTG0_OTG_CONTROL +
					  otg * OTG_STRIDE);

		printf("dm-offline: OTG%d control 0x%08x after off\n", otg, control);
		assert(!(control & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK));
	}
	assert(rt_display_off(&fixture_pdev, &report) == 0);	/* nothing showing */

	/* The same on the DisplayPort connector. */
	r = rt_display_show(&fixture_pdev, "DP-2", RT_DISPLAY_PATTERN_WHITE, &report);
	printf("dm-offline: display show DP-2 -> %d (commit %d), fb %ux%u at 0x%llx\n", r,
	       report.commit_status, report.fb_width, report.fb_height,
	       (unsigned long long)report.fb_gpu_addr);
	assert(r == 0 && report.showing && fb_pixel(&report, 960, 540) == 0xffffff);
	c = report_connector(&report, "DP-2");
	assert(c && c->lit && c->lit_width == 1920 && c->lit_height == 1080);
	assert(!report_connector(&report, "HDMI-A-1")->lit);
	assert(otg_with_timing(2200, 1125) >= 0 && hubp_scanning(report.fb_gpu_addr) >= 0);
	assert(rt_display_off(&fixture_pdev, &report) == 0 && report.restore_status == 0);
	assert(sysfs_text("drm/card0/card0-DP-2/enabled", text, sizeof(text)) > 0);
	assert(!strcmp(text, "disabled\n"));

	/* Every connected output (the default), another pattern, then the
	 * session-close path. */
	r = rt_display_show(&fixture_pdev, NULL, RT_DISPLAY_PATTERN_GRADIENT, &report);
	printf("dm-offline: display show (all) gradient -> %d\n", r);
	assert(r == 0 && report.showing && report.pattern == RT_DISPLAY_PATTERN_GRADIENT);
	{
		const struct rt_display_connector *a = report_connector(&report, "HDMI-A-1");
		const struct rt_display_connector *b = report_connector(&report, "DP-2");

		/* Both connected outputs, each on its own CRTC. */
		assert(a && b && a->lit && b->lit && a->crtc != b->crtc);
	}
	assert(fb_pixel(&report, 0, 0) == 0x0000ff && fb_pixel(&report, 1919, 0) == 0xff0000);
	r = rt_display_show(&fixture_pdev, "card0-HDMI-A-1", RT_DISPLAY_PATTERN_WHITE, &report);
	assert(r == 0 && report.showing && fb_pixel(&report, 960, 540) == 0xffffff);
	rt_display_stop();
	assert(!rt_display_showing());
	assert(sysfs_text("drm/card0/card0-HDMI-A-1/enabled", text, sizeof(text)) > 0);
	assert(!strcmp(text, "disabled\n"));
	printf("dm-offline: %lu client file(s) closed\n", fixture_files_closed);
	assert(fixture_files_closed >= 6);

	/* An output for a display agent: the exact mode or nothing. */
	r = rt_display_output(&fixture_pdev, "HDMI-A-1", 1920, 1080, 59000, &report);
	printf("dm-offline: display output 1920x1080@59.000 -> %d\n", r);
	assert(r == -EINVAL && !report.showing && !rt_display_showing());
	assert(rt_display_output(&fixture_pdev, "DP-1", 1920, 1080, 60000, &report) == -ENOENT);
	/* The output pipeline copies with SDMA, which this fixture has none
	 * of: refused after the mode was found (test-display-pipeline runs
	 * it on a fixture with both). */
	r = rt_display_output(&fixture_pdev, "HDMI-A-1", 1920, 1080, 60000, &report);
	printf("dm-offline: display output without SDMA -> %d\n", r);
	assert(r == -ENODEV && !report.showing && !rt_display_showing());
	assert(rt_display_present(&fixture_pdev, NULL, NULL, 0, NULL, 0, 0, NULL) == -EINVAL);

	/* The GPU leaves the bus while the pattern is on screen: nothing more
	 * is driven, and turning it off commits nothing. */
	r = rt_display_show(&fixture_pdev, "HDMI-A-1", RT_DISPLAY_PATTERN_BARS, &report);
	assert(r == 0 && report.showing);
	writes = dcn401_reg_writes;
	assert(rt_removal_begin(&fixture_pdev) == 0);
	assert(rt_display_probe(&fixture_pdev, &report) == -ENODEV);
	assert(rt_display_show(&fixture_pdev, NULL, RT_DISPLAY_PATTERN_WHITE, &report) == -ENODEV);
	r = rt_display_off(&fixture_pdev, &report);
	printf("dm-offline: display off after removal -> %d, %lu register writes\n", r,
	       dcn401_reg_writes - writes);
	assert(r == 0 && !rt_display_showing());
	rt_removal_end();

	drm_dev_unregister(ddev);
	assert(linuxu_sysfs_read(&fixture_dev.kobj, "drm/card0/card0-HDMI-A-1/status", text,
				 sizeof(text), 0, NULL) == -ENOENT);
	assert(linuxu_sysfs_list(&fixture_dev.kobj, "drm", text, sizeof(text), 0, NULL) == -ENOENT);
}

static int stage(const char *name, int ret)
{
	printf("dm-offline: %-12s -> %d\n", name, ret);
	return ret;
}

int main(void)
{
	struct amdgpu_ip_block block = { 0 };
	int r;

	assert(linuxu_sysinfo_init() == 0);
	assert(linuxu_timer_service_init() == 0);
	assert(linuxu_workqueue_init() == 0);
	assert(fw_table_register_embedded() == 0);
	assert(linuxu_module_init_gpu_buddy_module_init() == 0);
	assert(linuxu_module_init_drm_core_init() == 0);
	assert(linuxu_module_init_drm_sched_fence_slab_init() == 0);
	assert(amdgpu_sync_init() == 0);
	fixture_init();
	block.adev = adev;
	block.version = &dm_ip_block;

	/* dm_early_init: the VBIOS object header is found, DCN 4.0.1 gets
	 * four CRTCs/HPDs/DIGs and the DMCUB image is requested and
	 * validated. */
	r = stage("early_init", block.version->funcs->early_init(&block));
	assert(r == 0);
	assert(adev->dc_enabled);
	assert(adev->mode_info.num_crtc == 4 && adev->mode_info.num_hpd == 4);
	assert(adev->dm.dmub_fw && adev->dm.dmub_fw->size > 0);
	printf("dm-offline: DMCUB image %zu bytes\n", adev->dm.dmub_fw->size);

	/* dm_sw_init: the CGS device and the DMUB service for DCN401 are
	 * created from the real firmware, the region layout is computed and
	 * the DMUB framebuffer is a kernel BO in (fixture) VRAM. */
	r = stage("sw_init", block.version->funcs->sw_init(&block));
	assert(r == 0);
	assert(adev->dm.cgs_device && adev->dm.dmub_srv && adev->dm.dmub_fb_info);
	assert(adev->dm.dmub_srv->asic == DMUB_ASIC_DCN401);
	assert(adev->dm.dmub_bo_gpu_addr >= adev->gmc.vram_start &&
	       adev->dm.dmub_bo_gpu_addr <= adev->gmc.vram_end);
	printf("dm-offline: DMCUB firmware 0x%08x, DMUB framebuffer at GPU 0x%llx\n",
	       adev->dm.dmcub_fw_version,
	       (unsigned long long)adev->dm.dmub_bo_gpu_addr);

	/* dm_hw_init -> amdgpu_dm_init: DM IRQ tables, dc_create() (BIOS
	 * parser, DCN401 resource pool, links for every VBIOS connector), DMUB
	 * hardware init, dc_hardware_init(), and the DRM CRTCs, planes,
	 * encoders and connectors. */
	r = stage("hw_init", block.version->funcs->hw_init(&block));
	assert(r == 0);
	assert(adev->dm.dc);
	assert(adev->dm.dc->ctx->dce_version == DCN_VERSION_4_01);
	printf("dm-offline: %s, %u link(s) (%zu VBIOS connectors + virtual)\n",
	       dce_version_to_string(adev->dm.dc->ctx->dce_version),
	       adev->dm.dc->link_count, DCN401_FIXTURE_PATHS);
	assert(adev->dm.dc->link_count == DCN401_FIXTURE_PATHS + 1);

	/* The DMUB service took the firmware into its framebuffer and reset
	 * the controller; nothing executes it here, so the auto-load wait and
	 * every later DMUB command time out (logged above). */
	assert(adev->dm.dmub_srv->hw_init);

	/* DRM connectors, one per VBIOS path, all disconnected (no sink). */
	{
		struct drm_connector_list_iter iter;
		struct drm_connector *connector;
		unsigned int dp = 0, hdmi = 0;

		drm_connector_list_iter_begin(adev_to_drm(adev), &iter);
		drm_for_each_connector_iter(connector, &iter) {
			printf("dm-offline: connector %s status %d\n", connector->name,
			       connector->status);
			dp += connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort;
			hdmi += connector->connector_type == DRM_MODE_CONNECTOR_HDMIA;
			assert(connector->status != connector_status_connected);
		}
		drm_connector_list_iter_end(&iter);
		assert(dp == 3 && hdmi == 1);
	}
	assert(adev_to_drm(adev)->mode_config.num_crtc == adev->mode_info.num_crtc);
	printf("dm-offline: %d CRTC(s), %u register reads, %u writes\n",
	       adev_to_drm(adev)->mode_config.num_crtc,
	       (unsigned)dcn401_reg_reads, (unsigned)dcn401_reg_writes);

	display_test();

	/* Teardown in amdgpu_device_ip_fini() order. */
	r = stage("hw_fini", block.version->funcs->hw_fini(&block));
	assert(r == 0 && !adev->dm.dc);
	r = stage("sw_fini", block.version->funcs->sw_fini(&block));
	assert(r == 0 && !adev->dm.dmub_srv);

	printf("dm-offline: done\n");
	return 0;
}
