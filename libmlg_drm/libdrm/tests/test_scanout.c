/* Client framebuffers on the display output, from the client's side: a
 * libdrm-mlg program (this platform's headers, BSD request encoding) that
 * opens the primary node, reads the KMS objects, puts VRAM buffers in
 * framebuffers and shows them on the output the driver runs, through
 * libmlg_drm and the loopback transport into the unmodified upstream
 * DRM/amdgpu and amdgpu_dm of the fixture device (test_scanout_main.c).
 *
 * Covers: the primary node is not DRM master and the driver's modesets
 * still work with it open; KMS queries (resources, connectors with EDID,
 * planes, their types and IN_FORMATS); framebuffers of dma-buf imports;
 * LX_SCANOUT admission (attach to a connector, a second client, presents
 * from a client that is not attached, framebuffers of no file); presents on
 * the primary plane (the desktop's frames wait and are counted), a present
 * while the previous one is not committed (EBUSY), the overlay plane over
 * the desktop, the syncobj signalling at the flip, TEST; hide (a window off
 * the monitor) and a cropped present after it; detach; the output
 * relit under an attached client.
 *
 * This file is a library of its own linked against libdrm-mlg (as
 * test_libdrm_mlg.c is); the main program brings the fixture up. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <amdgpu_drm.h>
#include <amdgpu.h>

#include "test_scanout.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", \
	__FILE__, __LINE__, #c, errno); scanout_fx_report(); exit(1); } } while (0)

#define W	1920u
#define H	1080u

struct buffer {
	amdgpu_bo_handle bo;
	uint32_t *cpu;
	uint32_t handle;	/* in the primary file */
	uint32_t fb;
	uint32_t width, height, pitch;
};

/* A linear XRGB8888 buffer in VRAM filled with @value, as a framebuffer of
 * the primary file @card: allocated on the render node, shared by dma-buf. */
static void buffer_make(amdgpu_device_handle dev, int card, uint32_t width, uint32_t height,
			uint32_t value, struct buffer *b)
{
	struct amdgpu_bo_alloc_request req = {
		.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM,
		.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
		.phys_alignment = 64 * 1024,
	};
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR };
	uint32_t dmabuf;

	memset(b, 0, sizeof(*b));
	b->width = width;
	b->height = height;
	b->pitch = (width * 4 + 255) & ~255u;
	req.alloc_size = (uint64_t)b->pitch * height;
	CHECK(amdgpu_bo_alloc(dev, &req, &b->bo) == 0);
	CHECK(amdgpu_bo_cpu_map(b->bo, (void **)&b->cpu) == 0);
	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			b->cpu[(size_t)y * (b->pitch / 4) + x] = value;
	/* Unmapped again: a CPU mapping pins the buffer where it is, and
	 * scanout pins it in contiguous VRAM (amdgpu_dm's prepare_fb). */
	CHECK(amdgpu_bo_cpu_unmap(b->bo) == 0);
	b->cpu = NULL;
	CHECK(amdgpu_bo_export(b->bo, amdgpu_bo_handle_type_dma_buf_fd, &dmabuf) == 0);
	CHECK(drmPrimeFDToHandle(card, (int)dmabuf, &b->handle) == 0);
	close((int)dmabuf);
	handles[0] = b->handle;
	pitches[0] = b->pitch;
	CHECK(drmModeAddFB2WithModifiers(card, width, height, DRM_FORMAT_XRGB8888, handles, pitches,
					 offsets, modifiers, &b->fb, DRM_MODE_FB_MODIFIERS) == 0);
}

static void buffer_free(int card, struct buffer *b)
{
	CHECK(drmModeRmFB(card, b->fb) == 0);
	CHECK(drmCloseBufferHandle(card, b->handle) == 0);
	CHECK(amdgpu_bo_free(b->bo) == 0);
}

static int64_t deadline_ns(unsigned int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + (int64_t)ms * 1000000ll;
}

static int flip_wait(int card, uint32_t sync, unsigned int ms)
{
	return drmSyncobjWait(card, &sync, 1, deadline_ns(ms),
			      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
}

/* Until @cond holds, for up to 3 s: the flip's fence signals at the vblank,
 * and the fixture's DC finishes programming a plane change (whose register
 * waits time out on the fixture) after it. */
#define EVENTUALLY(cond) ({ int ok_ = 0; \
	for (int i_ = 0; i_ < 300 && !(ok_ = !!(cond)); i_++) usleep(10000); ok_; })

/* Exactly one display pipe shows it (DC picks which). */
static int one_pipe(unsigned int mask)
{
	return mask && !(mask & (mask - 1));
}

static struct mlg_lx_scanout request(uint32_t op)
{
	return (struct mlg_lx_scanout){ .version = MLG_LX_SCANOUT_VERSION, .op = op };
}

static int present(int card, const struct buffer *b, uint32_t layer, uint32_t sync,
		   int32_t x, int32_t y, uint32_t w, uint32_t h, struct mlg_lx_scanout_state *st)
{
	struct mlg_lx_scanout req = request(MLG_LX_SCANOUT_PRESENT);

	req.fd = card;
	req.layer = layer;
	req.fb_id = b->fb;
	req.syncobj = sync;
	req.dst_x = x;
	req.dst_y = y;
	req.dst_w = w;
	req.dst_h = h;
	return drmMlgScanout(&req, st);
}

/* The plane's "type" property (DRM_PLANE_TYPE_*) and whether its
 * IN_FORMATS allow XRGB8888 linear. */
static uint64_t plane_type(int card, uint32_t plane, int *linear_xrgb)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(card, plane, DRM_MODE_OBJECT_PLANE);
	uint64_t type = ~0ull;

	CHECK(props);
	*linear_xrgb = 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(card, props->props[i]);

		CHECK(p);
		if (!strcmp(p->name, "type")) {
			CHECK(drm_property_type_is(p, DRM_MODE_PROP_ENUM) && p->count_enums >= 3);
			type = props->prop_values[i];
		} else if (!strcmp(p->name, "IN_FORMATS")) {
			drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(card, (uint32_t)props->prop_values[i]);
			drmModeFormatModifierIterator it = { 0 };

			CHECK(blob);
			while (drmModeFormatModifierBlobIterNext(blob, &it))
				if (it.fmt == DRM_FORMAT_XRGB8888 && it.mod == DRM_FORMAT_MOD_LINEAR)
					*linear_xrgb = 1;
			drmModeFreePropertyBlob(blob);
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return type;
}

int test_scanout(void)
{
	struct mlg_lx_scanout_state st;
	struct mlg_lx_scanout req;
	amdgpu_device_handle dev;
	uint32_t major, minor, hdmi = 0, sync;
	struct buffer a, b, c, small;
	int card, render, card2;

	/* The primary node: open, and not master; neither is a second file. */
	card = drmFileOpen("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	CHECK(card >= 0 && drmGetNodeTypeFromFd(card) == DRM_NODE_PRIMARY);
	CHECK(!scanout_fx_has_master());
	card2 = drmFileOpen("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	CHECK(card2 >= 0 && !scanout_fx_has_master());
	close(card2);
	render = drmFileOpen("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	CHECK(render >= 0);
	CHECK(amdgpu_device_initialize(render, &major, &minor, &dev) == 0);
	CHECK(drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0);

	/* KMS objects. */
	{
		drmModeResPtr res = drmModeGetResources(card);
		drmModePlaneResPtr planes;
		int primaries = 0, overlays = 0, cursors = 0;

		CHECK(res && res->count_crtcs >= 1 && res->count_connectors >= 1 && res->count_encoders >= 1);
		CHECK(res->max_width >= W && res->max_height >= H);
		for (int i = 0; i < res->count_connectors; i++) {
			drmModeConnectorPtr conn = drmModeGetConnector(card, res->connectors[i]);
			const char *type;

			CHECK(conn);
			type = drmModeGetConnectorTypeName(conn->connector_type);
			CHECK(type);
			if (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA && conn->connector_type_id == 1) {
				uint32_t display = 0;

				CHECK(conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0);
				CHECK(conn->modes[0].hdisplay && conn->count_encoders >= 1);
				hdmi = conn->connector_id;
				/* No macOS display is that monitor in this test. */
				CHECK(drmMlgConnectorDisplay(card, hdmi, &display) == -ENOENT);
			}
			drmModeFreeConnector(conn);
		}
		CHECK(hdmi);
		for (int i = 0; i < res->count_encoders; i++) {
			drmModeEncoderPtr enc = drmModeGetEncoder(card, res->encoders[i]);

			CHECK(enc && enc->possible_crtcs);
			drmModeFreeEncoder(enc);
		}
		for (int i = 0; i < res->count_crtcs; i++) {
			drmModeCrtcPtr crtc = drmModeGetCrtc(card, res->crtcs[i]);

			CHECK(crtc && crtc->crtc_id == res->crtcs[i]);
			drmModeFreeCrtc(crtc);
		}
		planes = drmModeGetPlaneResources(card);
		CHECK(planes && planes->count_planes >= 2);
		for (uint32_t i = 0; i < planes->count_planes; i++) {
			drmModePlanePtr p = drmModeGetPlane(card, planes->planes[i]);
			int linear;
			uint64_t type;

			CHECK(p && p->count_formats > 0 && p->possible_crtcs);
			type = plane_type(card, p->plane_id, &linear);
			if (type == DRM_PLANE_TYPE_PRIMARY) {
				primaries++;
				CHECK(linear);
			} else if (type == DRM_PLANE_TYPE_OVERLAY) {
				overlays++;
				CHECK(linear);
			} else {
				CHECK(type == DRM_PLANE_TYPE_CURSOR);
				cursors++;
			}
			drmModeFreePlane(p);
		}
		printf("scanout: %d CRTCs, %d connectors, planes: %d primary, %d overlay, %d cursor\n",
		       res->count_crtcs, res->count_connectors, primaries, overlays, cursors);
		CHECK(primaries == res->count_crtcs && overlays >= 1);
		drmModeFreePlaneResources(planes);
		drmModeFreeResources(res);
	}
	/* Requests that change the display are not carried. */
	{
		struct drm_mode_crtc set = { 0 };

		CHECK(drmIoctl(card, DRM_IOCTL_MODE_SETCRTC, &set) == -1 && errno == ENOTTY);
	}

	buffer_make(dev, card, W, H, 0x00aa0001u, &a);
	buffer_make(dev, card, W, H, 0x00aa0002u, &b);
	buffer_make(dev, card, W, H, TEST_SCANOUT_LEFT_ON_SCREEN, &c);
	buffer_make(dev, card, 640, 360, 0x00aa0004u, &small);
	CHECK(drmSyncobjCreate(card, 0, &sync) == 0);

	/* Admission. */
	CHECK(present(card, &a, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == -EACCES);
	CHECK(st.output && !st.attached && st.width == W && st.height == H);
	CHECK(!strcmp(st.connector, "HDMI-A-1") && st.connector_id == hdmi && st.refresh_mhz > 59000);
	req = request(MLG_LX_SCANOUT_ATTACH);
	strcpy(req.connector, "DP-1");
	CHECK(drmMlgScanout(&req, &st) == -ENOENT && !st.attached);
	strcpy(req.connector, "HDMI-A-1");
	CHECK(drmMlgScanout(&req, &st) == 0 && st.attached);
	CHECK(st.crtc_id && st.primary_plane_id && st.overlay_plane_id);
	CHECK(drmMlgScanout(&req, &st) == 0 && st.attached);	/* again: the same */
	CHECK(scanout_fx_second_client_attach() == -EBUSY);
	req = request(MLG_LX_SCANOUT_PRESENT);
	req.fd = render;	/* framebuffers are the primary file's */
	CHECK(drmMlgScanout(&req, &st) == -EBADF);
	CHECK(present(card, &(struct buffer){ .fb = 9999 }, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0,
		      &st) == -ENOENT);
	CHECK(present(card, &a, 7, sync, 0, 0, 0, 0, &st) == -EINVAL);
	req = request(MLG_LX_SCANOUT_TEST);
	req.fd = card;
	req.layer = MLG_LX_LAYER_PRIMARY;
	req.fb_id = a.fb;
	CHECK(drmMlgScanout(&req, &st) == 0);
	req.src_w = W + 1;
	req.src_h = H;
	CHECK(drmMlgScanout(&req, &st) == -ERANGE);

	/* The primary plane: the client's frame instead of the desktop. */
	CHECK(present(card, &a, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
	CHECK(flip_wait(card, sync, 2000) == 0);
	req = request(MLG_LX_SCANOUT_STATE);
	CHECK(drmMlgScanout(&req, &st) == 0 && !st.error && st.presents == 1 && st.flips == 1);
	CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0001u))));
	{
		uint64_t received, flipped, received2, flipped2;
		int error;

		/* Desktop frames wait while the client has the plane. */
		scanout_fx_desktop_stats(&received, &flipped, &error);
		CHECK(scanout_fx_desktop_present(0x00203040u) == 0);
		CHECK(scanout_fx_desktop_present(0x00304050u) == 0);
		usleep(100000);
		scanout_fx_desktop_stats(&received2, &flipped2, &error);
		CHECK(!error && received2 == received + 2 && flipped2 == flipped);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0001u))));
		req = request(MLG_LX_SCANOUT_STATE);
		CHECK(drmMlgScanout(&req, &st) == 0);
		CHECK(st.layer == MLG_LX_LAYER_PRIMARY && st.presents == 1 && st.flips == 1 &&
		      st.desktop_held == 2);

		CHECK(present(card, &b, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
		CHECK(flip_wait(card, sync, 2000) == 0);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0002u))));

		/* A smaller framebuffer scaled to the whole mode. */
		CHECK(present(card, &small, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
		CHECK(flip_wait(card, sync, 2000) == 0);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0004u))));
		CHECK(present(card, &b, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
		CHECK(flip_wait(card, sync, 2000) == 0);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0002u))));

		/* No flip can happen: one frame is committed and waits for the
		 * vblank, the worker holds the next one until that flip, a
		 * third waits in the mailbox, a fourth is refused. */
		scanout_fx_hold_vblank(1);
		usleep(40000);
		{
			uint32_t s2, s3, s4;
			int r2, r3, r4;

			CHECK(drmSyncobjCreate(card, 0, &s2) == 0 && drmSyncobjCreate(card, 0, &s3) == 0 &&
			      drmSyncobjCreate(card, 0, &s4) == 0);
			CHECK(present(card, &a, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
			usleep(20000);
			r2 = present(card, &b, MLG_LX_LAYER_PRIMARY, s2, 0, 0, 0, 0, &st);
			usleep(20000);
			r3 = present(card, &a, MLG_LX_LAYER_PRIMARY, s3, 0, 0, 0, 0, &st);
			r4 = present(card, &b, MLG_LX_LAYER_PRIMARY, s4, 0, 0, 0, 0, &st);
			printf("scanout: presents while no flip can happen: %d %d %d\n", r2, r3, r4);
			CHECK(r2 == 0 && r3 == 0 && r4 == -EBUSY);
			CHECK(flip_wait(card, sync, 0) == -ETIME);	/* not flipped yet */
			scanout_fx_hold_vblank(0);
			CHECK(flip_wait(card, sync, 2000) == 0 && flip_wait(card, s2, 2000) == 0 &&
			      flip_wait(card, s3, 2000) == 0);
			CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0001u))));
			CHECK(drmSyncobjDestroy(card, s2) == 0 && drmSyncobjDestroy(card, s3) == 0 &&
			      drmSyncobjDestroy(card, s4) == 0);
		}

		/* The overlay plane over the desktop: the held desktop frame
		 * goes back on the primary plane. */
		CHECK(present(card, &small, MLG_LX_LAYER_OVERLAY, sync, 100, 100, 640, 360, &st) == 0);
		CHECK(flip_wait(card, sync, 2000) == 0);
		usleep(100000);
		scanout_fx_desktop_stats(&received2, &flipped2, &error);
		CHECK(!error && flipped2 == flipped + 1);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00304050u))));
		{
			unsigned int pipes;

			CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00aa0004u))));
			pipes = scanout_fx_pipes_showing(0x00aa0004u);

			printf("scanout: the overlay on pipe mask 0x%x, the desktop on 0x%x\n", pipes,
			       scanout_fx_pipes_showing(0x00304050u));
			CHECK(one_pipe(pipes) && !(pipes & scanout_fx_pipes_showing(0x00304050u)));
		}
		/* Desktop frames flow while the client is on the overlay. */
		CHECK(scanout_fx_desktop_present(0x00405060u) == 0);
		usleep(100000);
		CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00405060u)) &&
				 scanout_fx_pipes_showing(0x00aa0004u)));
		/* The window moves. */
		CHECK(present(card, &small, MLG_LX_LAYER_OVERLAY, sync, 800, 500, 640, 360, &st) == 0);
		CHECK(flip_wait(card, sync, 2000) == 0);
		CHECK(EVENTUALLY(scanout_fx_pipes_showing(0x00aa0004u)));

		/* A window that leaves the monitor: HIDE takes the overlay off
		 * and leaves the client attached; its next present (cropped:
		 * the window back half over the left edge) shows it again. */
		{
			int32_t x, y;
			uint32_t w, h;

			req = request(MLG_LX_SCANOUT_HIDE);
			CHECK(drmMlgScanout(&req, &st) == 0 && st.attached && st.overlay_plane_id);
			CHECK(EVENTUALLY(!scanout_fx_pipes_showing(0x00aa0004u) &&
					 one_pipe(scanout_fx_pipes_showing(0x00405060u))));
			CHECK(!scanout_fx_overlay(&x, &y, &w, &h));
			CHECK(drmMlgScanout(&req, &st) == 0);	/* again: nothing to do */
			req = request(MLG_LX_SCANOUT_PRESENT);
			req.fd = card;
			req.layer = MLG_LX_LAYER_OVERLAY;
			req.fb_id = small.fb;
			req.syncobj = sync;
			req.src_x = 320;
			req.src_y = 0;
			req.src_w = 320;
			req.src_h = 360;
			req.dst_x = 0;
			req.dst_y = 200;
			req.dst_w = 320;
			req.dst_h = 360;
			CHECK(drmMlgScanout(&req, &st) == 0);
			CHECK(flip_wait(card, sync, 2000) == 0);
			CHECK(EVENTUALLY(scanout_fx_overlay(&x, &y, &w, &h) && x == 0 && y == 200 &&
					 w == 320 && h == 360));
			/* A crop past the framebuffer is refused, not trimmed. */
			req.op = MLG_LX_SCANOUT_TEST;
			req.src_x = 400;
			CHECK(drmMlgScanout(&req, &st) == -ERANGE);
		}

		/* Detach: the overlay goes; presents are refused. */
		req = request(MLG_LX_SCANOUT_DETACH);
		CHECK(drmMlgScanout(&req, &st) == 0 && !st.attached);
		CHECK(EVENTUALLY(!scanout_fx_pipes_showing(0x00aa0004u) &&
				 one_pipe(scanout_fx_pipes_showing(0x00405060u))));
		CHECK(present(card, &a, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == -EACCES);
	}

	/* The output relit while the client has the primary node open: the
	 * driver's modeset is not refused, and the new output has no client. */
	req = request(MLG_LX_SCANOUT_ATTACH);
	CHECK(drmMlgScanout(&req, &st) == 0 && st.attached);
	CHECK(scanout_fx_relight() == 0);
	CHECK(present(card, &a, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == -EACCES);
	CHECK(!st.attached && st.output);
	CHECK(scanout_fx_desktop_present(0x00506070u) == 0);
	usleep(100000);
	CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(0x00506070u))));

	/* Attached again, a frame left on the primary plane for the main
	 * program's check of the client's death. */
	CHECK(drmMlgScanout(&req, &st) == 0 && st.attached);
	CHECK(present(card, &c, MLG_LX_LAYER_PRIMARY, sync, 0, 0, 0, 0, &st) == 0);
	CHECK(flip_wait(card, sync, 2000) == 0);
	CHECK(EVENTUALLY(one_pipe(scanout_fx_pipes_showing(TEST_SCANOUT_LEFT_ON_SCREEN))));

	buffer_free(card, &a);
	buffer_free(card, &b);
	buffer_free(card, &small);
	/* c stays: it is on screen. */
	CHECK(amdgpu_device_deinitialize(dev) == 0);
	printf("scanout: client checks passed\n");
	return 0;
}
