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
 * It runs dm_early_init, dm_sw_init, dm_hw_init, dm_hw_fini and
 * dm_sw_fini as amdgpu_device_ip_init()/ip_fini() would. Everything up to
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
#include <rt/bootstrap.h>
#include <drm/drm_drv.h>
#include "amdgpu_ttm.h"
#include "amdgpu_reset.h"

extern const struct amdgpu_ip_block_version dm_ip_block;
extern const struct amdgpu_ip_block_version gmc_v12_0_ip_block;
extern int linuxu_workqueue_init(void);
extern int linuxu_timer_service_init(void);
extern int linuxu_sysinfo_init(void);
extern int fw_table_register_embedded(void);
extern int linuxu_module_init_drm_core_init(void);
extern int linuxu_module_init_gpu_buddy_module_init(void);

#define VBIOS_BYTES	(64 * 1024)
#define MMIO_BYTES	(64ULL << 20)

static uint8_t vbios[VBIOS_BYTES];
static uint32_t dce_base[6];
static size_t vbios_used = 0x400;

static uint16_t vb_alloc(size_t bytes)
{
	size_t at = (vbios_used + 3) & ~(size_t)3;

	assert(at + bytes < VBIOS_BYTES);
	vbios_used = at + bytes;
	return (uint16_t)at;
}

static void vb_header(uint16_t at, uint16_t size, uint8_t frev, uint8_t crev)
{
	struct atom_common_table_header *h = (void *)&vbios[at];

	h->structuresize = size;
	h->format_revision = frev;
	h->content_revision = crev;
}

/* One connector path: connector object, its first (internal) encoder, the
 * device tag, and a record list with a DDC line and an HPD pin. */
struct fixture_path {
	uint16_t connector;
	uint16_t encoder;
	uint16_t device_tag;
	uint8_t i2c_id;
	uint8_t hpd_pin;
};

static const struct fixture_path paths[] = {
	{ (GRAPH_OBJECT_TYPE_CONNECTOR << 12) | (GRAPH_OBJECT_ENUM_ID1 << 8) | CONNECTOR_OBJECT_ID_DISPLAYPORT,
	  (GRAPH_OBJECT_TYPE_ENCODER << 12) | (GRAPH_OBJECT_ENUM_ID1 << 8) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY,
	  ATOM_DISPLAY_DFP1_SUPPORT, 0x91, 1 },
	{ (GRAPH_OBJECT_TYPE_CONNECTOR << 12) | (GRAPH_OBJECT_ENUM_ID2 << 8) | CONNECTOR_OBJECT_ID_DISPLAYPORT,
	  (GRAPH_OBJECT_TYPE_ENCODER << 12) | (GRAPH_OBJECT_ENUM_ID2 << 8) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY,
	  ATOM_DISPLAY_DFP2_SUPPORT, 0x92, 2 },
	{ (GRAPH_OBJECT_TYPE_CONNECTOR << 12) | (GRAPH_OBJECT_ENUM_ID3 << 8) | CONNECTOR_OBJECT_ID_DISPLAYPORT,
	  (GRAPH_OBJECT_TYPE_ENCODER << 12) | (GRAPH_OBJECT_ENUM_ID1 << 8) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY1,
	  ATOM_DISPLAY_DFP3_SUPPORT, 0x93, 3 },
	{ (GRAPH_OBJECT_TYPE_CONNECTOR << 12) | (GRAPH_OBJECT_ENUM_ID1 << 8) | CONNECTOR_OBJECT_ID_HDMI_TYPE_A,
	  (GRAPH_OBJECT_TYPE_ENCODER << 12) | (GRAPH_OBJECT_ENUM_ID2 << 8) | ENCODER_OBJECT_ID_INTERNAL_UNIPHY1,
	  ATOM_DISPLAY_DFP4_SUPPORT, 0x94, 4 },
};
#define NPATHS (sizeof(paths) / sizeof(paths[0]))

static void build_vbios(void)
{
	struct atom_rom_header_v2_2 *rom;
	struct atom_master_data_table_v2_1 *mdt;
	struct display_object_info_table_v1_5 *obj;
	struct atom_gpio_pin_lut_v2_1 *lut;
	uint16_t rom_at, cmd_at, mdt_at, obj_at, lut_at;
	size_t obj_bytes, lut_bytes;

	vbios[0] = 0x55;
	vbios[1] = 0xaa;
	vbios[2] = VBIOS_BYTES / 512;			/* image size, 512 B units */
	memcpy(&vbios[0x30], " 761295520", 10);		/* ATOM_ATI_MAGIC */
	/* Identification strings (part number, name, date, version). */
	vbios[OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS] = 1;
	*(uint16_t *)&vbios[OFFSET_TO_GET_ATOMBIOS_STRING_START] = 0x200;
	memcpy(&vbios[0x200], "LINUXU-DCN401-FIXTURE\0\r\nlinuxu fixture VBIOS", 45);
	memcpy(&vbios[0x260], "ATOMBIOSBK-AMD VER000.000.000.000.000000", 41);
	memcpy(&vbios[OFFSET_TO_VBIOS_DATE], "10/02/26 12:00", 14);

	rom_at = vb_alloc(sizeof(*rom));
	*(uint16_t *)&vbios[0x48] = rom_at;		/* ATOM_ROM_TABLE_PTR */
	rom = (void *)&vbios[rom_at];
	vb_header(rom_at, sizeof(*rom), 2, 2);
	memcpy(rom->atom_bios_string, "ATOM", 4);

	cmd_at = vb_alloc(sizeof(struct atom_common_table_header) +
			  sizeof(struct atom_master_list_of_command_functions_v2_1));
	vb_header(cmd_at, sizeof(struct atom_common_table_header) +
		  sizeof(struct atom_master_list_of_command_functions_v2_1), 2, 1);
	rom->masterhwfunction_offset = cmd_at;

	mdt_at = vb_alloc(sizeof(*mdt));
	vb_header(mdt_at, sizeof(*mdt), 2, 1);
	rom->masterdatatable_offset = mdt_at;
	mdt = (void *)&vbios[mdt_at];

	/* Display paths, each followed by its record list. */
	obj_bytes = sizeof(*obj) + NPATHS * sizeof(struct atom_display_object_path_v3);
	obj_at = vb_alloc(obj_bytes);
	vb_header(obj_at, obj_bytes, 1, 5);
	obj = (void *)&vbios[obj_at];
	obj->number_of_path = NPATHS;
	for (size_t i = 0; i < NPATHS; i++) {
		struct atom_display_object_path_v3 *p = &obj->display_path[i];
		uint16_t rec_at = vb_alloc(sizeof(struct atom_i2c_record) +
					   sizeof(struct atom_hpd_int_record) + 1);
		struct atom_i2c_record *i2c = (void *)&vbios[rec_at];
		struct atom_hpd_int_record *hpd = (void *)(i2c + 1);

		obj->supporteddevices |= paths[i].device_tag;
		p->display_objid = paths[i].connector;
		p->encoderobjid = paths[i].encoder;
		p->device_tag = paths[i].device_tag;
		/* Record offsets are relative to the object info table. */
		p->disp_recordoffset = rec_at - obj_at;
		i2c->record_header.record_type = ATOM_I2C_RECORD_TYPE;
		i2c->record_header.record_size = sizeof(*i2c);
		i2c->i2c_id = paths[i].i2c_id;
		hpd->record_header.record_type = ATOM_HPD_INT_RECORD_TYPE;
		hpd->record_header.record_size = sizeof(*hpd);
		hpd->pin_id = paths[i].hpd_pin;
		hpd->plugin_pin_state = 1;
		*((uint8_t *)(hpd + 1)) = ATOM_RECORD_END_TYPE;
	}
	mdt->listOfdatatables.displayobjectinfo = obj_at;

	/* GPIO pins for the DDC lines and HPD pins named by the records. */
	lut_bytes = sizeof(*lut) + 2 * NPATHS * sizeof(struct atom_gpio_pin_assignment);
	lut_at = vb_alloc(lut_bytes);
	vb_header(lut_at, lut_bytes, 2, 1);
	lut = (void *)&vbios[lut_at];
	/* DDC lines DDC1..4 and HPD pins HPD1..4, at the DCN 4.0.1 GPIO
	 * registers (segment 2) the GPIO translation expects. */
	for (size_t i = 0; i < NPATHS; i++) {
		static const uint32_t ddc[] = { regDC_GPIO_DDC1_A, regDC_GPIO_DDC2_A,
						regDC_GPIO_DDC3_A, regDC_GPIO_DDC4_A };
		static const uint8_t hpd_shift[] = {
			DC_GPIO_HPD_A__DC_GPIO_HPD1_A__SHIFT, DC_GPIO_HPD_A__DC_GPIO_HPD2_A__SHIFT,
			DC_GPIO_HPD_A__DC_GPIO_HPD3_A__SHIFT, DC_GPIO_HPD_A__DC_GPIO_HPD4_A__SHIFT };

		lut->gpio_pin[2 * i].gpio_id = paths[i].i2c_id;
		lut->gpio_pin[2 * i].data_a_reg_index = dce_base[regDC_GPIO_DDC1_A_BASE_IDX] + ddc[i];
		lut->gpio_pin[2 * i + 1].gpio_id = paths[i].hpd_pin;
		lut->gpio_pin[2 * i + 1].data_a_reg_index = dce_base[regDC_GPIO_HPD_A_BASE_IDX] + regDC_GPIO_HPD_A;
		lut->gpio_pin[2 * i + 1].gpio_bitshift = hpd_shift[i];
	}
	mdt->listOfdatatables.gpio_pin_lut = lut_at;

	/* Firmware info v3.4, display controller info v4.5 (100 MHz DCE and
	 * DP PHY reference, 50 MHz I2C engine reference) and SMU info v4.0,
	 * the table versions a DCN 4.x board carries. */
	{
		uint16_t fw_at = vb_alloc(sizeof(struct atom_firmware_info_v3_4));
		uint16_t dce_at = vb_alloc(sizeof(struct atom_display_controller_info_v4_5));
		uint16_t smu_at = vb_alloc(sizeof(struct atom_smu_info_v4_0));
		struct atom_firmware_info_v3_4 *fw = (void *)&vbios[fw_at];
		struct atom_display_controller_info_v4_5 *dce = (void *)&vbios[dce_at];

		vb_header(fw_at, sizeof(*fw), 3, 4);
		fw->bootup_sclk_in10khz = 50000;
		fw->bootup_mclk_in10khz = 100000;
		vb_header(dce_at, sizeof(*dce), 4, 5);
		dce->dce_refclk_10khz = 10000;
		dce->dpphy_refclk_10khz = 10000;
		dce->i2c_engine_refclk_10khz = 5000;
		vb_header(smu_at, sizeof(struct atom_smu_info_v4_0), 4, 0);
		mdt->listOfdatatables.firmwareinfo = fw_at;
		mdt->listOfdatatables.dce_info = dce_at;
		mdt->listOfdatatables.smu_info = smu_at;
	}
}

/* Fixture IP base addresses (dword offsets) for DCE, NBIO and CLK, the
 * values IP discovery would supply. They only place the registers inside
 * the fixture register file. */
static uint32_t dce_base[6] = { 0x00000012, 0x000000C0, 0x000034C0, 0x00009000, 0x00380000, 0 };
static uint32_t nbio_base[] = { 0x00000000, 0x00000014, 0x00000D20, 0x00010400, 0x00390000, 0 };
static uint32_t clk_base[] = { 0x00016C00, 0x00016E00, 0x00017000, 0x00017200, 0x0001B000, 0x0001B200 };

static struct amdgpu_device *adev;

/* Register file: every register reads back what was last written. */
static uint32_t *fixture_regs;
static unsigned long fixture_reg_reads, fixture_reg_writes;

static u32 fixture_rreg(struct amdgpu_device *a, u32 byte_offset)
{
	(void)a;
	__atomic_add_fetch(&fixture_reg_reads, 1, __ATOMIC_RELAXED);
	return byte_offset < MMIO_BYTES ? fixture_regs[byte_offset / 4] : 0;
}

static void fixture_wreg(struct amdgpu_device *a, u32 byte_offset, u32 v)
{
	(void)a;
	__atomic_add_fetch(&fixture_reg_writes, 1, __ATOMIC_RELAXED);
	if (byte_offset < MMIO_BYTES)
		fixture_regs[byte_offset / 4] = v;
}
static struct device fixture_dev;
static struct pci_dev fixture_pdev;
static struct amdgpu_reset_domain fixture_reset_domain;
static const struct drm_driver fixture_driver = {
	.name = "amdgpu",
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_RENDER,
};

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
	assert(amdgpu_ttm_init(adev) == 0);
	adev->mman.aper_base_kaddr = fixture_vram;
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
	adev->ip_versions[DCE_HWIP][0] = IP_VERSION(4, 0, 1);
	adev->ip_versions[MMHUB_HWIP][0] = IP_VERSION(4, 1, 0);
	adev->reg_offset[DCE_HWIP][0] = dce_base;
	adev->reg_offset[NBIO_HWIP][0] = nbio_base;
	adev->reg_offset[CLK_HWIP][0] = clk_base;
	adev->firmware.load_type = AMDGPU_FW_LOAD_PSP;
	amdgpu_set_init_level(adev, AMDGPU_INIT_LEVEL_DEFAULT);

	/* No direct MMIO aperture: every register access takes amdgpu's
	 * indirect path (adev->reg.pcie), which the fixture register file
	 * implements. */
	fixture_regs = calloc(1, MMIO_BYTES);
	assert(fixture_regs);
	adev->rmmio_size = 0;
	spin_lock_init(&adev->mmio_idx_lock);
	spin_lock_init(&adev->reg.pcie.lock);
	adev->reg.pcie.rreg = fixture_rreg;
	adev->reg.pcie.wreg = fixture_wreg;

	/* Fuse: this DCN has its DMCUB microcontroller (CC_DC_PIPE_DIS). */
	WREG32(dce_base[regCC_DC_PIPE_DIS_BASE_IDX] + regCC_DC_PIPE_DIS,
	       CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK);
	assert(RREG32(dce_base[regCC_DC_PIPE_DIS_BASE_IDX] + regCC_DC_PIPE_DIS) ==
	       CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK);
	fixture_ttm_init();
	/* The IH block's sw_init: the device interrupt and its sources. */
	printf("dm-offline: amdgpu_irq_init -> %d\n", amdgpu_irq_init(adev));

	build_vbios();
	adev->bios = vbios;
	adev->bios_size = VBIOS_BYTES;
	adev->is_atom_fw = true;
	assert(amdgpu_atombios_init(adev) == 0);
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
	       adev->dm.dc->link_count, NPATHS);
	assert(adev->dm.dc->link_count == NPATHS + 1);

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
	       (unsigned)fixture_reg_reads, (unsigned)fixture_reg_writes);

	/* Teardown in amdgpu_device_ip_fini() order. */
	r = stage("hw_fini", block.version->funcs->hw_fini(&block));
	assert(r == 0 && !adev->dm.dc);
	r = stage("sw_fini", block.version->funcs->sw_fini(&block));
	assert(r == 0 && !adev->dm.dmub_srv);

	printf("dm-offline: done\n");
	return 0;
}
