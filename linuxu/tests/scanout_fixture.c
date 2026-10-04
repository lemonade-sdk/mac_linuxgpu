/* The display output on the fixture device, for clients of LX_SCANOUT
 * (test_scanout.h): the CS fixture device with the fixture DCN 4.0.1
 * display of test-display-pipeline, its flip interrupts on a 16 ms
 * "vblank", an output lit on HDMI-A-1 and fed desktop frames as a display
 * agent feeds it, and what only the kernel side can see: which buffer each
 * display pipe scans out, the master state of the DRM device, a second
 * client. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int usleep(unsigned int usec);

#include <linux/pci.h>
#include <drm/drm_auth.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_plane.h>
#include <drm/drm_framebuffer.h>
#include <rt/display.h>
#include <rt/lx_files.h>
#include <rt/surface.h>

#include "amdgpu.h"
#include "amdgpu_dm.h"
#include "core_types.h"
#include "dcn/dcn_4_1_0_offset.h"
#include "ivsrcid/dcn/irqsrcs_dcn_1_0.h"
#include "soc15_ih_clientid.h"
#include "mlg_drm.h"
#include "cs_fixture.h"
#include "dcn401_fixture.h"
#include "lx_loopback.h"
#include "test_scanout.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

extern const struct amdgpu_ip_block_version dm_ip_block;
extern int fw_table_register_embedded(void);
extern int drm_edid_override_set(struct drm_connector *connector, const void *edid, size_t size);
extern u64 ktime_get_ns(void);

#define PAGE	(16u * 1024u)

static struct amdgpu_device *adev;
static struct pci_dev *pdev;

static void display_init(struct amdgpu_device *a)
{
	struct amdgpu_ip_block block = { .adev = a, .version = &dm_ip_block };

	drm_mode_config_init(adev_to_drm(a));
	a->firmware.load_type = AMDGPU_FW_LOAD_PSP;
	a->ip_versions[MMHUB_HWIP][0] = IP_VERSION(4, 1, 0);
	dcn401_fixture_attach(a);
	dcn401_fixture_vbios(a);
	CHECK(block.version->funcs->early_init(&block) == 0);
	CHECK(block.version->funcs->sw_init(&block) == 0);
	CHECK(block.version->funcs->hw_init(&block) == 0);
}

/* ---- the fixture's vblank ---- */
static volatile int vblank_run = 1, vblank_hold;
static pthread_t vblank_thread;

static void raise_irq(unsigned int src_id)
{
	struct amdgpu_irq_src *src = adev->irq.client[SOC15_IH_CLIENTID_DCE].sources ?
		adev->irq.client[SOC15_IH_CLIENTID_DCE].sources[src_id] : NULL;
	struct amdgpu_iv_entry entry = { .client_id = SOC15_IH_CLIENTID_DCE, .src_id = src_id };

	if (src && src->funcs && src->funcs->process)
		src->funcs->process(adev, src, &entry);
}

static void *vblank_main(void *arg)
{
	(void)arg;
	while (vblank_run) {
		usleep(16000);
		if (vblank_hold)
			continue;
		for (int i = 0; i < 4; i++) {
			uint32_t reg = dcn401_dce_base[regOTG0_OTG_STATUS_FRAME_COUNT_BASE_IDX] +
				       regOTG0_OTG_STATUS_FRAME_COUNT + i * (regOTG1_OTG_H_TOTAL - regOTG0_OTG_H_TOTAL);

			WREG32(reg, RREG32(reg) + 1);
		}
		for (int i = 0; i < 4; i++)
			raise_irq(DCN_1_0__SRCID__HUBP0_FLIP_INTERRUPT + i);
	}
	return NULL;
}

void scanout_fx_hold_vblank(int hold)
{
	vblank_hold = hold;
}

/* ---- what each pipe scans out ---- */

uint32_t scanout_fx_pipe_pixel(unsigned int pipe, uint64_t offset)
{
	const uint32_t stride = regHUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS -
				regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS;
	uint32_t lo = RREG32(dcn401_dce_base[regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_BASE_IDX] +
			     regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS + pipe * stride);
	uint32_t hi = RREG32(dcn401_dce_base[regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_BASE_IDX] +
			     regHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH + pipe * stride);
	uint64_t address = ((uint64_t)hi << 32) | lo;
	uint8_t *fb;

	if (!address)
		return 0;
	fb = cs_fixture_vram_host(address + offset);
	return fb ? *(uint32_t *)fb : 0;
}

/* Pipes whose surface starts with @value: a bit per pipe. */
unsigned int scanout_fx_pipes_showing(uint32_t value)
{
	const struct dc *dc = adev->dm.dc;
	unsigned int mask = 0;

	for (unsigned int i = 0; i < 4; i++) {
		const struct pipe_ctx *p = &dc->current_state->res_ctx.pipe_ctx[i];

		/* A pipe DC took off keeps its last address in the register. */
		if (!p->plane_state)
			continue;
		if (scanout_fx_pipe_pixel(i, 0) == value)
			mask |= 1u << i;
	}
	return mask;
}

/* The CRTC that drives HDMI-A-1. */
static struct drm_crtc *output_crtc(void)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_crtc *crtc = NULL;

	drm_connector_list_iter_begin(adev_to_drm(adev), &iter);
	drm_for_each_connector_iter(connector, &iter)
		if (!strcmp(connector->name, "HDMI-A-1") && connector->state)
			crtc = connector->state->crtc;
	drm_connector_list_iter_end(&iter);
	return crtc;
}

int scanout_fx_primary_fb(uint32_t *width, uint32_t *height, uint64_t *modifier)
{
	struct drm_crtc *crtc = output_crtc();
	const struct drm_plane_state *ps = crtc ? crtc->primary->state : NULL;

	if (!ps || !ps->fb)
		return 0;
	*width = ps->fb->width;
	*height = ps->fb->height;
	*modifier = ps->fb->modifier;
	return 1;
}

int scanout_fx_overlay(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h)
{
	struct drm_crtc *crtc = output_crtc();
	struct drm_plane *plane;

	if (!crtc)
		return 0;
	drm_for_each_plane(plane, adev_to_drm(adev)) {
		const struct drm_plane_state *ps = plane->state;

		if (plane->type != DRM_PLANE_TYPE_OVERLAY || !ps || ps->crtc != crtc || !ps->fb)
			continue;
		*x = ps->crtc_x;
		*y = ps->crtc_y;
		*w = ps->crtc_w;
		*h = ps->crtc_h;
		return 1;
	}
	return 0;
}

int scanout_fx_has_master(void)
{
	return adev_to_drm(adev)->master != NULL;
}

/* ---- desktop frames, as a display agent presents them ---- */
#define W	1920u
#define H	1080u
#define PITCH	(W * 4)

static uint8_t *desk_mem;
static uint64_t desk_size;
static struct rt_surface *desk;
static uint32_t desk_handle;

static void desk_release(void *context)
{
	(void)context;
}

static void desk_import(void)
{
	static struct rt_surface_provider provider = { desk_release, NULL };
	struct rt_surface_segment seg;

	desk_size = ((uint64_t)PITCH * H + PAGE - 1) / PAGE * PAGE;
	desk_mem = aligned_alloc(PAGE, desk_size);
	CHECK(desk_mem);
	seg = (struct rt_surface_segment){ (uint64_t)(uintptr_t)desk_mem, desk_size };
	CHECK(rt_surface_import(pdev, &seg, 1, desk_size, W, H, PITCH, &provider, &desk) == 0);
	desk_handle = rt_surface_add(1, desk);
	CHECK(desk_handle);
}

int scanout_fx_desktop_present(uint32_t value)
{
	const struct rt_surface_rect all = { 0, 0, W, H };
	struct rt_display_present_stats st;

	for (uint64_t i = 0; i < (uint64_t)PITCH * H / 4; i++)
		((uint32_t *)desk_mem)[i] = value;
	return rt_display_present(pdev, rt_surface_get_hold(1, desk_handle), &all, 1, ktime_get_ns(),
				  &st);
}

void scanout_fx_desktop_stats(uint64_t *received, uint64_t *flipped, int *error)
{
	struct rt_display_present_stats st;

	CHECK(rt_display_stats(pdev, &st) == 0);
	*received = st.frames_received;
	*flipped = st.frames_flipped;
	*error = st.error;
}

/* The output turned off and lit again, with the client's primary file open:
 * the driver's own modeset (drm_client_modeset_commit) works because no
 * client file is DRM master. */
int scanout_fx_relight(void)
{
	struct rt_display_report report;
	int r = rt_display_off(pdev, &report);

	if (r)
		return r;
	return rt_display_output(pdev, "HDMI-A-1", W, H, 60000, &report);
}

/* A second Linux-file client tries to attach while the test's client is. */
int scanout_fx_second_client_attach(void)
{
	struct mlg_lx_scanout req = { .version = MLG_LX_SCANOUT_VERSION, .op = MLG_LX_SCANOUT_ATTACH };
	struct mlg_lx_scanout_state state;
	struct rt_lx_client *other;
	int r;

	CHECK(rt_lx_client_create(pdev, 0, "second", &other) == 0);
	rt_lx_client_set_display(other, &rt_display_lx_hooks);
	r = rt_lx_scanout(other, &req, &state);
	rt_lx_client_destroy(other);
	return r;
}

struct pci_dev *scanout_fixture_start(uint32_t desktop)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector, *hdmi = NULL;
	struct rt_display_report report;

	CHECK(fw_table_register_embedded() == 0);
	cs_fixture_sdma_instances = 2;
	cs_fixture_display = display_init;
	pdev = cs_fixture_init();
	adev = cs_fixture_adev();

	dcn401_fixture_build_edid();
	drm_connector_list_iter_begin(adev_to_drm(adev), &iter);
	drm_for_each_connector_iter(connector, &iter)
		if (!strcmp(connector->name, "HDMI-A-1"))
			hdmi = connector;
	drm_connector_list_iter_end(&iter);
	CHECK(hdmi);
	hdmi->force = DRM_FORCE_ON;
	CHECK(drm_edid_override_set(hdmi, dcn401_fixture_edid, sizeof(dcn401_fixture_edid)) == 0);
	CHECK(rt_display_probe(pdev, &report) == 0);

	CHECK(pthread_create(&vblank_thread, NULL, vblank_main, NULL) == 0);
	CHECK(rt_display_output(pdev, "HDMI-A-1", W, H, 60000, &report) == 0);
	desk_import();
	CHECK(scanout_fx_desktop_present(desktop) == 0);
	return pdev;
}

void scanout_fixture_finish(void)
{
	struct rt_display_report report;

	CHECK(rt_display_off(pdev, &report) == 0);
	CHECK(rt_surface_remove(1, desk_handle) == 0);
	vblank_run = 0;
	pthread_join(vblank_thread, NULL);
	cs_fixture_stop();
}
