/* libdrm for mac_linuxgpu: the KMS queries and framebuffer requests of
 * xf86drmMode.h over drmIoctl. Each query runs as libdrm runs it: once
 * with zero counts for the sizes, then with arrays of those sizes, again
 * while the sizes change between the two. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "xf86drmMode.h"

#define U642VOID(x) ((void *)(uintptr_t)(x))
#define VOID2U64(x) ((uint64_t)(uintptr_t)(x))

/* drmIoctl's convention, as libdrm's mode calls return it: 0 or -errno. */
static int mode_ioctl(int fd, unsigned long request, void *arg)
{
	return drmIoctl(fd, request, arg) ? -errno : 0;
}

/* calloc of @count elements, a valid pointer for zero. */
static void *array(size_t count, size_t size)
{
	return calloc(count ? count : 1, size);
}

/* ---- resources ---- */

drmModeResPtr drmModeGetResources(int fd)
{
	struct drm_mode_card_res res, counts;
	drmModeResPtr r;

	for (;;) {
		memset(&res, 0, sizeof(res));
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
			return NULL;
		counts = res;
		uint32_t *fbs = array(res.count_fbs, sizeof(uint32_t));
		uint32_t *crtcs = array(res.count_crtcs, sizeof(uint32_t));
		uint32_t *connectors = array(res.count_connectors, sizeof(uint32_t));
		uint32_t *encoders = array(res.count_encoders, sizeof(uint32_t));

		if (!fbs || !crtcs || !connectors || !encoders) {
			free(fbs);
			free(crtcs);
			free(connectors);
			free(encoders);
			errno = ENOMEM;
			return NULL;
		}
		res.fb_id_ptr = VOID2U64(fbs);
		res.crtc_id_ptr = VOID2U64(crtcs);
		res.connector_id_ptr = VOID2U64(connectors);
		res.encoder_id_ptr = VOID2U64(encoders);
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
			free(fbs);
			free(crtcs);
			free(connectors);
			free(encoders);
			return NULL;
		}
		if (res.count_fbs > counts.count_fbs || res.count_crtcs > counts.count_crtcs ||
		    res.count_connectors > counts.count_connectors ||
		    res.count_encoders > counts.count_encoders) {
			/* Objects appeared in between (a hotplugged MST port). */
			free(fbs);
			free(crtcs);
			free(connectors);
			free(encoders);
			continue;
		}
		r = calloc(1, sizeof(*r));
		if (!r) {
			free(fbs);
			free(crtcs);
			free(connectors);
			free(encoders);
			errno = ENOMEM;
			return NULL;
		}
		r->count_fbs = (int)res.count_fbs;
		r->fbs = fbs;
		r->count_crtcs = (int)res.count_crtcs;
		r->crtcs = crtcs;
		r->count_connectors = (int)res.count_connectors;
		r->connectors = connectors;
		r->count_encoders = (int)res.count_encoders;
		r->encoders = encoders;
		r->min_width = res.min_width;
		r->max_width = res.max_width;
		r->min_height = res.min_height;
		r->max_height = res.max_height;
		return r;
	}
}

void drmModeFreeResources(drmModeResPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->fbs);
	free(ptr->crtcs);
	free(ptr->connectors);
	free(ptr->encoders);
	free(ptr);
}

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtc_id)
{
	struct drm_mode_crtc crtc = { .crtc_id = crtc_id };
	drmModeCrtcPtr r;

	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc))
		return NULL;
	r = calloc(1, sizeof(*r));
	if (!r) {
		errno = ENOMEM;
		return NULL;
	}
	r->crtc_id = crtc.crtc_id;
	r->buffer_id = crtc.fb_id;
	r->x = crtc.x;
	r->y = crtc.y;
	r->mode_valid = (int)crtc.mode_valid;
	if (crtc.mode_valid) {
		memcpy(&r->mode, &crtc.mode, sizeof(r->mode));
		r->width = crtc.mode.hdisplay;
		r->height = crtc.mode.vdisplay;
	}
	r->gamma_size = (int)crtc.gamma_size;
	return r;
}

void drmModeFreeCrtc(drmModeCrtcPtr ptr)
{
	free(ptr);
}

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id)
{
	struct drm_mode_get_encoder enc = { .encoder_id = encoder_id };
	drmModeEncoderPtr r;

	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc))
		return NULL;
	r = calloc(1, sizeof(*r));
	if (!r) {
		errno = ENOMEM;
		return NULL;
	}
	r->encoder_id = enc.encoder_id;
	r->encoder_type = enc.encoder_type;
	r->crtc_id = enc.crtc_id;
	r->possible_crtcs = enc.possible_crtcs;
	r->possible_clones = enc.possible_clones;
	return r;
}

void drmModeFreeEncoder(drmModeEncoderPtr ptr)
{
	free(ptr);
}

/* ---- connectors ---- */

_Static_assert(sizeof(drmModeModeInfo) == sizeof(struct drm_mode_modeinfo), "mode info layout");

drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connector_id)
{
	struct drm_mode_get_connector conn, counts;
	drmModeConnectorPtr r;

	for (;;) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connector_id;
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
			return NULL;
		counts = conn;
		uint32_t *props = array(conn.count_props, sizeof(uint32_t));
		uint64_t *values = array(conn.count_props, sizeof(uint64_t));
		struct drm_mode_modeinfo *modes = array(conn.count_modes, sizeof(*modes));
		uint32_t *encoders = array(conn.count_encoders, sizeof(uint32_t));

		if (!props || !values || !modes || !encoders) {
			free(props);
			free(values);
			free(modes);
			free(encoders);
			errno = ENOMEM;
			return NULL;
		}
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connector_id;
		conn.count_props = counts.count_props;
		conn.props_ptr = VOID2U64(props);
		conn.prop_values_ptr = VOID2U64(values);
		conn.count_modes = counts.count_modes;
		conn.modes_ptr = VOID2U64(modes);
		conn.count_encoders = counts.count_encoders;
		conn.encoders_ptr = VOID2U64(encoders);
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) {
			free(props);
			free(values);
			free(modes);
			free(encoders);
			return NULL;
		}
		if (conn.count_props > counts.count_props || conn.count_modes > counts.count_modes ||
		    conn.count_encoders > counts.count_encoders) {
			free(props);
			free(values);
			free(modes);
			free(encoders);
			continue;
		}
		r = calloc(1, sizeof(*r));
		if (!r) {
			free(props);
			free(values);
			free(modes);
			free(encoders);
			errno = ENOMEM;
			return NULL;
		}
		r->connector_id = conn.connector_id;
		r->encoder_id = conn.encoder_id;
		r->connection = (drmModeConnection)conn.connection;
		r->mmWidth = conn.mm_width;
		r->mmHeight = conn.mm_height;
		/* The kernel's subpixel order counts from 0; libdrm's from 1. */
		r->subpixel = (drmModeSubPixel)(conn.subpixel + 1);
		r->connector_type = conn.connector_type;
		r->connector_type_id = conn.connector_type_id;
		r->count_props = (int)conn.count_props;
		r->props = props;
		r->prop_values = values;
		r->count_modes = (int)conn.count_modes;
		r->modes = (drmModeModeInfoPtr)modes;
		r->count_encoders = (int)conn.count_encoders;
		r->encoders = encoders;
		return r;
	}
}

drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t connector_id)
{
	/* Only a master's query probes, and these files never are. */
	return drmModeGetConnector(fd, connector_id);
}

void drmModeFreeConnector(drmModeConnectorPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->encoders);
	free(ptr->prop_values);
	free(ptr->props);
	free(ptr->modes);
	free(ptr);
}

const char *drmModeGetConnectorTypeName(uint32_t connector_type)
{
	/* drm_connector_enum_list's names. */
	switch (connector_type) {
	case DRM_MODE_CONNECTOR_Unknown: return "Unknown";
	case DRM_MODE_CONNECTOR_VGA: return "VGA";
	case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite: return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
	case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
	case DRM_MODE_CONNECTOR_Component: return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV: return "TV";
	case DRM_MODE_CONNECTOR_eDP: return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
	case DRM_MODE_CONNECTOR_DSI: return "DSI";
	case DRM_MODE_CONNECTOR_DPI: return "DPI";
	case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
	case DRM_MODE_CONNECTOR_SPI: return "SPI";
	case DRM_MODE_CONNECTOR_USB: return "USB";
	default: return NULL;
	}
}

/* ---- planes ---- */

drmModePlaneResPtr drmModeGetPlaneResources(int fd)
{
	struct drm_mode_get_plane_res res;
	drmModePlaneResPtr r;

	for (;;) {
		uint32_t count;

		memset(&res, 0, sizeof(res));
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res))
			return NULL;
		count = res.count_planes;
		uint32_t *planes = array(count, sizeof(uint32_t));

		if (!planes) {
			errno = ENOMEM;
			return NULL;
		}
		res.plane_id_ptr = VOID2U64(planes);
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res)) {
			free(planes);
			return NULL;
		}
		if (res.count_planes > count) {
			free(planes);
			continue;
		}
		r = calloc(1, sizeof(*r));
		if (!r) {
			free(planes);
			errno = ENOMEM;
			return NULL;
		}
		r->count_planes = res.count_planes;
		r->planes = planes;
		return r;
	}
}

void drmModeFreePlaneResources(drmModePlaneResPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->planes);
	free(ptr);
}

drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id)
{
	struct drm_mode_get_plane plane;
	drmModePlanePtr r;

	for (;;) {
		uint32_t count;

		memset(&plane, 0, sizeof(plane));
		plane.plane_id = plane_id;
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane))
			return NULL;
		count = plane.count_format_types;
		uint32_t *formats = array(count, sizeof(uint32_t));

		if (!formats) {
			errno = ENOMEM;
			return NULL;
		}
		plane.format_type_ptr = VOID2U64(formats);
		if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane)) {
			free(formats);
			return NULL;
		}
		if (plane.count_format_types > count) {
			free(formats);
			continue;
		}
		r = calloc(1, sizeof(*r));
		if (!r) {
			free(formats);
			errno = ENOMEM;
			return NULL;
		}
		r->count_formats = plane.count_format_types;
		r->formats = formats;
		r->plane_id = plane.plane_id;
		r->crtc_id = plane.crtc_id;
		r->fb_id = plane.fb_id;
		r->possible_crtcs = plane.possible_crtcs;
		r->gamma_size = plane.gamma_size;
		return r;
	}
}

void drmModeFreePlane(drmModePlanePtr ptr)
{
	if (!ptr)
		return;
	free(ptr->formats);
	free(ptr);
}

/* ---- properties ---- */

drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id,
						      uint32_t object_type)
{
	struct drm_mode_obj_get_properties p;
	drmModeObjectPropertiesPtr r;

	for (;;) {
		uint32_t count;

		memset(&p, 0, sizeof(p));
		p.obj_id = object_id;
		p.obj_type = object_type;
		if (mode_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p))
			return NULL;
		count = p.count_props;
		uint32_t *props = array(count, sizeof(uint32_t));
		uint64_t *values = array(count, sizeof(uint64_t));

		if (!props || !values) {
			free(props);
			free(values);
			errno = ENOMEM;
			return NULL;
		}
		p.props_ptr = VOID2U64(props);
		p.prop_values_ptr = VOID2U64(values);
		if (mode_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &p)) {
			free(props);
			free(values);
			return NULL;
		}
		if (p.count_props > count) {
			free(props);
			free(values);
			continue;
		}
		r = calloc(1, sizeof(*r));
		if (!r) {
			free(props);
			free(values);
			errno = ENOMEM;
			return NULL;
		}
		r->count_props = p.count_props;
		r->props = props;
		r->prop_values = values;
		return r;
	}
}

void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->props);
	free(ptr->prop_values);
	free(ptr);
}

drmModePropertyPtr drmModeGetProperty(int fd, uint32_t property_id)
{
	struct drm_mode_get_property prop, counts;
	drmModePropertyPtr r;

	memset(&prop, 0, sizeof(prop));
	prop.prop_id = property_id;
	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop))
		return NULL;
	counts = prop;
	uint64_t *values = array(prop.count_values, sizeof(uint64_t));
	struct drm_mode_property_enum *enums = array(prop.count_enum_blobs, sizeof(*enums));

	if (!values || !enums) {
		free(values);
		free(enums);
		errno = ENOMEM;
		return NULL;
	}
	prop.values_ptr = VOID2U64(values);
	prop.enum_blob_ptr = VOID2U64(enums);
	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop)) {
		free(values);
		free(enums);
		return NULL;
	}
	r = calloc(1, sizeof(*r));
	if (!r) {
		free(values);
		free(enums);
		errno = ENOMEM;
		return NULL;
	}
	r->prop_id = prop.prop_id;
	r->flags = prop.flags;
	memcpy(r->name, prop.name, sizeof(r->name));
	r->name[DRM_PROP_NAME_LEN - 1] = 0;
	r->count_values = (int)counts.count_values;
	r->values = values;
	if (prop.flags & (DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BITMASK)) {
		r->count_enums = (int)counts.count_enum_blobs;
		r->enums = enums;
	} else {
		free(enums);
	}
	/* A blob property's value is its current blob; there is no list. */
	return r;
}

void drmModeFreeProperty(drmModePropertyPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->values);
	free(ptr->enums);
	free(ptr->blob_ids);
	free(ptr);
}

drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
	struct drm_mode_get_blob blob = { .blob_id = blob_id };
	drmModePropertyBlobPtr r;
	void *data;

	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob))
		return NULL;
	data = array(blob.length, 1);
	if (!data) {
		errno = ENOMEM;
		return NULL;
	}
	blob.data = VOID2U64(data);
	if (mode_ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob)) {
		free(data);
		return NULL;
	}
	r = calloc(1, sizeof(*r));
	if (!r) {
		free(data);
		errno = ENOMEM;
		return NULL;
	}
	r->id = blob.blob_id;
	r->length = blob.length;
	r->data = data;
	return r;
}

void drmModeFreePropertyBlob(drmModePropertyBlobPtr ptr)
{
	if (!ptr)
		return;
	free(ptr->data);
	free(ptr);
}

bool drmModeFormatModifierBlobIterNext(const drmModePropertyBlobRes *blob,
				       drmModeFormatModifierIterator *iter)
{
	const struct drm_format_modifier_blob *fmt_mod_blob;
	const struct drm_format_modifier *mods;
	const uint32_t *formats;

	if (!blob || !iter || !blob->data || blob->length < sizeof(*fmt_mod_blob))
		return false;
	fmt_mod_blob = blob->data;
	if (fmt_mod_blob->formats_offset > blob->length ||
	    fmt_mod_blob->modifiers_offset > blob->length ||
	    (uint64_t)fmt_mod_blob->formats_offset + (uint64_t)fmt_mod_blob->count_formats * 4 >
	    blob->length ||
	    (uint64_t)fmt_mod_blob->modifiers_offset +
	    (uint64_t)fmt_mod_blob->count_modifiers * sizeof(*mods) > blob->length)
		return false;
	formats = (const uint32_t *)((const uint8_t *)fmt_mod_blob + fmt_mod_blob->formats_offset);
	mods = (const struct drm_format_modifier *)((const uint8_t *)fmt_mod_blob +
						    fmt_mod_blob->modifiers_offset);
	/* Each (format, modifier) pair the blob allows, modifier by modifier
	 * within each format. */
	for (; iter->fmt_idx < fmt_mod_blob->count_formats; iter->fmt_idx++, iter->mod_idx = 0) {
		for (; iter->mod_idx < fmt_mod_blob->count_modifiers; iter->mod_idx++) {
			const struct drm_format_modifier *m = &mods[iter->mod_idx];
			uint32_t bit = iter->fmt_idx;

			if (bit < m->offset || bit >= m->offset + 64)
				continue;
			if (!(m->formats & (1ull << (bit - m->offset))))
				continue;
			iter->fmt = formats[iter->fmt_idx];
			iter->mod = m->modifier;
			iter->mod_idx++;
			return true;
		}
	}
	return false;
}

/* ---- framebuffers ---- */

int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
			       const uint32_t bo_handles[4], const uint32_t pitches[4],
			       const uint32_t offsets[4], const uint64_t modifier[4],
			       uint32_t *buf_id, uint32_t flags)
{
	struct drm_mode_fb_cmd2 f = {
		.width = width, .height = height, .pixel_format = pixel_format, .flags = flags,
	};
	int r;

	if (!bo_handles || !pitches || !offsets || !buf_id)
		return -EINVAL;
	memcpy(f.handles, bo_handles, sizeof(f.handles));
	memcpy(f.pitches, pitches, sizeof(f.pitches));
	memcpy(f.offsets, offsets, sizeof(f.offsets));
	if (modifier)
		memcpy(f.modifier, modifier, sizeof(f.modifier));
	r = mode_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f);
	if (r)
		return r;
	*buf_id = f.fb_id;
	return 0;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
		  const uint32_t bo_handles[4], const uint32_t pitches[4],
		  const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags)
{
	return drmModeAddFB2WithModifiers(fd, width, height, pixel_format, bo_handles, pitches,
					  offsets, NULL, buf_id, flags);
}

int drmModeRmFB(int fd, uint32_t buffer_id)
{
	return mode_ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer_id);
}

int drmModeCloseFB(int fd, uint32_t buffer_id)
{
	struct drm_mode_closefb c = { .fb_id = buffer_id };

	return mode_ioctl(fd, DRM_IOCTL_MODE_CLOSEFB, &c);
}
