/* libdrm for mac_linuxgpu: the driver's display output (xf86drm.h,
 * "the driver's display output"): LX_SCANOUT for primary-node proxies, and
 * the macOS display that stands for a connector's monitor. */
#include <errno.h>
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
