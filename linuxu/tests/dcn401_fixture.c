/* A fixture DCN 4.0.1 display on a fixture amdgpu device (dcn401_fixture.h):
 * the parts test-dm-offline and the display pipeline test share. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amdgpu.h"
#include "atomfirmware.h"
#include "atom.h"
#include "amdgpu_atombios.h"
#include "ObjectID.h"
#include "dcn/dcn_4_1_0_offset.h"
#include "dcn/dcn_4_1_0_sh_mask.h"
#include "dcn401_fixture.h"

/* Fixture IP base addresses (dword offsets) for DCE, NBIO and CLK, the
 * values IP discovery would supply. They only place the registers inside
 * the fixture register file. */
uint32_t dcn401_dce_base[6] = { 0x00000012, 0x000000C0, 0x000034C0, 0x00009000, 0x00380000, 0 };
static uint32_t nbio_base[] = { 0x00000000, 0x00000014, 0x00000D20, 0x00010400, 0x00390000, 0 };
static uint32_t clk_base[] = { 0x00016C00, 0x00016E00, 0x00017000, 0x00017200, 0x0001B000, 0x0001B200 };

#define VBIOS_BYTES	(64 * 1024)
#define MMIO_BYTES	(64ULL << 20)

static uint8_t vbios[VBIOS_BYTES];
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
		lut->gpio_pin[2 * i].data_a_reg_index = dcn401_dce_base[regDC_GPIO_DDC1_A_BASE_IDX] + ddc[i];
		lut->gpio_pin[2 * i + 1].gpio_id = paths[i].hpd_pin;
		lut->gpio_pin[2 * i + 1].data_a_reg_index = dcn401_dce_base[regDC_GPIO_HPD_A_BASE_IDX] + regDC_GPIO_HPD_A;
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


/* Register file: every register reads back what was last written. */
static uint32_t *fixture_regs;
unsigned long dcn401_reg_reads, dcn401_reg_writes;

static u32 fixture_rreg(struct amdgpu_device *a, u32 byte_offset)
{
	(void)a;
	__atomic_add_fetch(&dcn401_reg_reads, 1, __ATOMIC_RELAXED);
	return byte_offset < MMIO_BYTES ? fixture_regs[byte_offset / 4] : 0;
}

static void fixture_wreg(struct amdgpu_device *a, u32 byte_offset, u32 v)
{
	(void)a;
	__atomic_add_fetch(&dcn401_reg_writes, 1, __ATOMIC_RELAXED);
	if (byte_offset < MMIO_BYTES)
		fixture_regs[byte_offset / 4] = v;
}

void dcn401_fixture_attach(struct amdgpu_device *adev)
{
	adev->ip_versions[DCE_HWIP][0] = IP_VERSION(4, 0, 1);
	adev->reg_offset[DCE_HWIP][0] = dcn401_dce_base;
	adev->reg_offset[NBIO_HWIP][0] = nbio_base;
	adev->reg_offset[CLK_HWIP][0] = clk_base;
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
	WREG32(dcn401_dce_base[regCC_DC_PIPE_DIS_BASE_IDX] + regCC_DC_PIPE_DIS,
	       CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK);
	assert(RREG32(dcn401_dce_base[regCC_DC_PIPE_DIS_BASE_IDX] + regCC_DC_PIPE_DIS) ==
	       CC_DC_PIPE_DIS__DC_DMCUB_ENABLE_MASK);
}

void dcn401_fixture_vbios(struct amdgpu_device *adev)
{
	build_vbios();
	adev->bios = vbios;
	adev->bios_size = VBIOS_BYTES;
	adev->is_atom_fw = true;
	assert(amdgpu_atombios_init(adev) == 0);
}

/* Synthetic EDIDs (EDID 1.4, 1920x1080@60 preferred, CEA-861 timing at
 * 148.5 MHz): one with a CTA-861 extension carrying an HDMI vendor block
 * for the HDMI connector, and its base block alone for a DisplayPort
 * connector. They describe no real monitor. */
uint8_t dcn401_fixture_edid[256];
uint8_t dcn401_fixture_edid_dp[128];

static void edid_checksum(uint8_t *block)
{
	uint8_t sum = 0;

	for (int i = 0; i < 127; i++)
		sum += block[i];
	block[127] = (uint8_t)(0x100 - sum);
}

void dcn401_fixture_build_edid(void)
{
	static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	static const uint8_t dtd_1080p[18] = { 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
					       0x2c, 0x45, 0x00, 0x58, 0x54, 0x21, 0x00, 0x00, 0x1e };
	static const uint8_t chroma[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
	static const uint8_t range[18] = { 0x00, 0x00, 0x00, 0xfd, 0x00, 0x38, 0x4c, 0x1e, 0x53, 0x11,
					   0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
	uint8_t *b = dcn401_fixture_edid, *x = dcn401_fixture_edid + 128;

	memcpy(b, header, 8);
	b[8] = 0x31; b[9] = 0xd8;		/* "LNX" */
	b[10] = 0x01;				/* product 1 */
	b[16] = 1; b[17] = 2026 - 1990;		/* week, year */
	b[18] = 1; b[19] = 4;			/* EDID 1.4 */
	b[20] = 0xa2;				/* digital, 8 bpc, HDMI-a */
	b[21] = 60; b[22] = 34;			/* cm */
	b[23] = 120;				/* gamma 2.2 */
	b[24] = 0x02;				/* preferred timing is native */
	memcpy(&b[25], chroma, 10);
	b[35] = 0x20;				/* 640x480@60 */
	for (int i = 38; i < 54; i++)
		b[i] = 0x01;			/* no standard timings */
	memcpy(&b[54], dtd_1080p, 18);
	memcpy(&b[72], "\0\0\0\xfc\0LINUXU TEST\n ", 18);
	memcpy(&b[90], range, 18);
	b[108 + 3] = 0x10;			/* dummy descriptor */
	b[126] = 1;				/* one extension */
	edid_checksum(b);

	x[0] = 0x02; x[1] = 0x03;		/* CTA-861 revision 3 */
	x[4] = 0x42; x[5] = 0x90; x[6] = 0x01;	/* video: VIC 16 (native), VIC 1 */
	x[7] = 0x65; x[8] = 0x03; x[9] = 0x0c; x[10] = 0x00; x[11] = 0x10; x[12] = 0x00; /* HDMI VSDB, 1.0.0.0 */
	x[2] = 13;				/* no DTDs: they would start here */
	x[3] = 0x00;
	edid_checksum(x);

	memcpy(dcn401_fixture_edid_dp, dcn401_fixture_edid, 128);
	dcn401_fixture_edid_dp[20] = 0xa5;		/* digital, 8 bpc, DisplayPort */
	dcn401_fixture_edid_dp[126] = 0;		/* no extension */
	memcpy(&dcn401_fixture_edid_dp[72], "\0\0\0\xfc\0LINUXU DP\n   ", 18);
	edid_checksum(dcn401_fixture_edid_dp);
}

