/* libdrm for mac_linuxgpu: the driver's display output (xf86drm.h,
 * "the driver's display output"): LX_SCANOUT for primary-node proxies, and
 * the macOS display that stands for a connector's monitor. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreGraphics/CoreGraphics.h>

#include "mlg_drm.h"
#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_internal.h"

int drmMlgScanout(const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state)
{
	struct mlg_lx_scanout copy;

	if (!req || !state)
		return -EINVAL;
	copy = *req;
	if (req->op == MLG_LX_SCANOUT_TEST || req->op == MLG_LX_SCANOUT_PRESENT) {
		if (drm_file_node_type(req->fd) != DRM_NODE_PRIMARY) {
			memset(state, 0, sizeof(*state));
			return -EBADF;
		}
		copy.fd = drm_file_driver_fd(req->fd);
		if (copy.fd < 0) {
			memset(state, 0, sizeof(*state));
			return -EBADF;
		}
	}
	return mlg_scanout(&copy, state) ? -errno : 0;
}

int drmMlgEdidIdentity(const void *edid, size_t length, uint32_t *vendor, uint32_t *product,
		       uint32_t *serial)
{
	static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	const uint8_t *b = edid;
	uint8_t sum = 0;

	if (!b || length < 128 || memcmp(b, header, sizeof(header)))
		return -EINVAL;
	for (int i = 0; i < 128; i++)
		sum += b[i];
	if (sum)
		return -EINVAL;
	*vendor = (uint32_t)b[8] << 8 | b[9];
	*product = (uint32_t)b[10] | (uint32_t)b[11] << 8;
	*serial = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 |
		  (uint32_t)b[15] << 24;
	if (*serial)
		return 0;
	/* The serial string descriptor (0xff), up to its newline, without
	 * surrounding blanks, hashed with 32-bit FNV-1a. */
	for (int at = 54; at < 126; at += 18) {
		const uint8_t *d = b + at;
		size_t start = 5, end = 5;
		uint32_t h = 2166136261u;

		if (d[0] || d[1] || d[3] != 0xff)
			continue;
		while (end < 18 && d[end] != 0x0a)
			end++;
		while (start < end && (d[start] == ' ' || d[start] == '\t'))
			start++;
		while (end > start && (d[end - 1] == ' ' || d[end - 1] == '\t'))
			end--;
		if (start == end)
			break;
		for (size_t i = start; i < end; i++)
			h = (h ^ d[i]) * 16777619u;
		*serial = h;
		break;
	}
	return 0;
}

/* The connector's EDID property blob. */
static drmModePropertyBlobPtr connector_edid(int fd, uint32_t connector_id, int *error)
{
	drmModeObjectPropertiesPtr props;
	drmModePropertyBlobPtr blob = NULL;
	uint64_t blob_id = 0;

	*error = -ENOENT;
	props = drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
	if (!props) {
		*error = -errno;
		return NULL;
	}
	for (uint32_t i = 0; i < props->count_props && !blob_id; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);

		if (p && !strcmp(p->name, "EDID"))
			blob_id = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	if (blob_id) {
		blob = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
		if (!blob)
			*error = -errno;
	}
	return blob;
}

int drmMlgConnectorDisplay(int fd, uint32_t connector_id, uint32_t *display_id)
{
	CGDirectDisplayID displays[32];
	uint32_t count = 0, vendor, product, serial, found = 0, matches = 0;
	drmModePropertyBlobPtr edid;
	int r;

	if (!display_id)
		return -EINVAL;
	edid = connector_edid(fd, connector_id, &r);
	if (!edid)
		return r;	/* no EDID: nothing to know the monitor by */
	r = drmMlgEdidIdentity(edid->data, edid->length, &vendor, &product, &serial);
	drmModeFreePropertyBlob(edid);
	if (r)
		return r;
	if (CGGetOnlineDisplayList(32, displays, &count) != kCGErrorSuccess)
		return -EIO;
	for (uint32_t i = 0; i < count; i++) {
		if (CGDisplayVendorNumber(displays[i]) != vendor ||
		    CGDisplayModelNumber(displays[i]) != product ||
		    CGDisplaySerialNumber(displays[i]) != serial)
			continue;
		found = displays[i];
		matches++;
	}
	if (!matches)
		return -ENOENT;
	if (matches > 1)
		return -EEXIST;
	*display_id = found;
	return 0;
}

static void place_whole(const struct drm_mlg_place_in *in, struct drm_mlg_placement *out)
{
	out->layer = MLG_LX_LAYER_PRIMARY;
	out->src_x = out->src_y = 0;
	out->src_w = in->image_w;
	out->src_h = in->image_h;
	out->dst_x = out->dst_y = 0;
	out->dst_w = in->crtc_w;
	out->dst_h = in->crtc_h;
}

int drmMlgPlaceWindow(const struct drm_mlg_place_in *in, struct drm_mlg_placement *out)
{
	memset(out, 0, sizeof(*out));
	if (!in->crtc_w || !in->crtc_h || !in->image_w || !in->image_h ||
	    in->mode < DRM_MLG_PLACE_AUTO || in->mode > DRM_MLG_PLACE_WINDOWED ||
	    (in->have_screen && (!(in->screen_w > 0) || !(in->screen_h > 0)))) {
		snprintf(out->why, sizeof(out->why), "no mode, image or screen size to place with");
		return -EINVAL;
	}
	if (in->mode == DRM_MLG_PLACE_FULLSCREEN) {
		place_whole(in, out);
		return 0;
	}
	if (!in->have_screen || !in->have_window) {
		/* Nothing to relate the window to: full screen, or the image's
		 * size centred when windowed was asked for. */
		if (in->mode == DRM_MLG_PLACE_AUTO) {
			place_whole(in, out);
			return 0;
		}
		out->layer = MLG_LX_LAYER_OVERLAY;
		out->src_w = in->image_w < in->crtc_w ? in->image_w : in->crtc_w;
		out->src_h = in->image_h < in->crtc_h ? in->image_h : in->crtc_h;
		out->dst_w = out->src_w;
		out->dst_h = out->src_h;
		out->dst_x = (int32_t)(in->crtc_w - out->dst_w) / 2;
		out->dst_y = (int32_t)(in->crtc_h - out->dst_h) / 2;
		return 0;
	}
	if (!(in->win_w > 0) || !(in->win_h > 0)) {
		snprintf(out->why, sizeof(out->why), "the window has no size");
		out->layer = 0;
		return 0;
	}

	/* The window in CRTC pixels: a screen point is crtc/screen pixels. */
	const double kx = in->crtc_w / in->screen_w, ky = in->crtc_h / in->screen_h;
	const double x0 = (in->win_x - in->screen_x) * kx, y0 = (in->win_y - in->screen_y) * ky;
	const double x1 = x0 + in->win_w * kx, y1 = y0 + in->win_h * ky;

	if (in->mode == DRM_MLG_PLACE_AUTO &&
	    (in->window_native_fullscreen ||
	     (x0 <= 0.5 && y0 <= 0.5 && x1 >= in->crtc_w - 0.5 && y1 >= in->crtc_h - 0.5))) {
		place_whole(in, out);
		return 0;
	}

	/* The part on the CRTC, and the part of the image that maps there. */
	const double vx0 = fmax(x0, 0), vy0 = fmax(y0, 0);
	const double vx1 = fmin(x1, in->crtc_w), vy1 = fmin(y1, in->crtc_h);
	if (vx1 <= vx0 || vy1 <= vy0) {
		snprintf(out->why, sizeof(out->why), "the window is off the monitor's screen");
		return 0;
	}
	const int32_t dx0 = (int32_t)lround(vx0), dy0 = (int32_t)lround(vy0);
	const int32_t dx1 = (int32_t)lround(vx1), dy1 = (int32_t)lround(vy1);
	if (dx1 - dx0 < DRM_MLG_MIN_VIEWPORT || dy1 - dy0 < DRM_MLG_MIN_VIEWPORT) {
		snprintf(out->why, sizeof(out->why),
			 "%dx%d pixels of the window are on the monitor, under DC's %d-pixel viewport",
			 dx1 - dx0, dy1 - dy0, DRM_MLG_MIN_VIEWPORT);
		return 0;
	}
	const double sx = in->image_w / (x1 - x0), sy = in->image_h / (y1 - y0);
	double sx0 = floor((dx0 - x0) * sx + 1e-6), sy0 = floor((dy0 - y0) * sy + 1e-6);
	double sx1 = ceil((dx1 - x0) * sx - 1e-6), sy1 = ceil((dy1 - y0) * sy - 1e-6);

	sx0 = fmax(sx0, 0);
	sy0 = fmax(sy0, 0);
	sx1 = fmin(sx1, in->image_w);
	sy1 = fmin(sy1, in->image_h);
	if (sx1 - sx0 < 1 || sy1 - sy0 < 1) {
		snprintf(out->why, sizeof(out->why), "no pixel of the image falls on the monitor");
		return 0;
	}
	out->layer = MLG_LX_LAYER_OVERLAY;
	out->dst_x = dx0;
	out->dst_y = dy0;
	out->dst_w = (uint32_t)(dx1 - dx0);
	out->dst_h = (uint32_t)(dy1 - dy0);
	out->src_x = (uint32_t)sx0;
	out->src_y = (uint32_t)sy0;
	out->src_w = (uint32_t)(sx1 - sx0);
	out->src_h = (uint32_t)(sy1 - sy0);
	return 0;
}
