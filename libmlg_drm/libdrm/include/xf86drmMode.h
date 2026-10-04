/* libdrm for mac_linuxgpu: the part of libdrm's mode-setting API
 * (xf86drmMode.h) that reads the KMS objects and creates framebuffers, over
 * libmlg_drm.
 *
 * Names, types and return conventions are libdrm's. The descriptors are
 * primary-node files ("/dev/dri/card0") of the GPU driver, which are never
 * DRM master: the driver's display output does all modesetting, and a
 * client shows its framebuffers there with drmMlgScanout (xf86drm.h). The
 * requests that would change the display (SETCRTC, SETPLANE, ATOMIC,
 * cursors, gamma, properties, leases) are therefore not provided.
 *
 * Connector queries never probe: a file that is not master gets the modes
 * of the last detection, as on Linux. */
#ifndef MLG_XF86DRMMODE_H
#define MLG_XF86DRMMODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drm.h>
#include <drm_mode.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_DISPLAY_INFO_LEN	32
#define DRM_CONNECTOR_NAME_LEN	32
#define DRM_DISPLAY_MODE_LEN	32
#define DRM_PROP_NAME_LEN	32

/* Values of a plane's "type" property. */
#define DRM_PLANE_TYPE_OVERLAY	0
#define DRM_PLANE_TYPE_PRIMARY	1
#define DRM_PLANE_TYPE_CURSOR	2

typedef struct _drmModeRes {
	int count_fbs;
	uint32_t *fbs;
	int count_crtcs;
	uint32_t *crtcs;
	int count_connectors;
	uint32_t *connectors;
	int count_encoders;
	uint32_t *encoders;
	uint32_t min_width, max_width;
	uint32_t min_height, max_height;
} drmModeRes, *drmModeResPtr;

/* struct drm_mode_modeinfo, field for field. */
typedef struct _drmModeModeInfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char name[DRM_DISPLAY_MODE_LEN];
} drmModeModeInfo, *drmModeModeInfoPtr;

typedef struct _drmModeCrtc {
	uint32_t crtc_id;
	uint32_t buffer_id;
	uint32_t x, y;
	uint32_t width, height;
	int mode_valid;
	drmModeModeInfo mode;
	int gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

typedef struct _drmModeEncoder {
	uint32_t encoder_id;
	uint32_t encoder_type;
	uint32_t crtc_id;
	uint32_t possible_crtcs;
	uint32_t possible_clones;
} drmModeEncoder, *drmModeEncoderPtr;

typedef enum {
	DRM_MODE_CONNECTED = 1,
	DRM_MODE_DISCONNECTED = 2,
	DRM_MODE_UNKNOWNCONNECTION = 3
} drmModeConnection;

typedef enum {
	DRM_MODE_SUBPIXEL_UNKNOWN = 1,
	DRM_MODE_SUBPIXEL_HORIZONTAL_RGB = 2,
	DRM_MODE_SUBPIXEL_HORIZONTAL_BGR = 3,
	DRM_MODE_SUBPIXEL_VERTICAL_RGB = 4,
	DRM_MODE_SUBPIXEL_VERTICAL_BGR = 5,
	DRM_MODE_SUBPIXEL_NONE = 6
} drmModeSubPixel;

typedef struct _drmModeConnector {
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t connector_type;
	uint32_t connector_type_id;
	drmModeConnection connection;
	uint32_t mmWidth, mmHeight;
	drmModeSubPixel subpixel;
	int count_modes;
	drmModeModeInfoPtr modes;
	int count_props;
	uint32_t *props;
	uint64_t *prop_values;
	int count_encoders;
	uint32_t *encoders;
} drmModeConnector, *drmModeConnectorPtr;

typedef struct _drmModeProperty {
	uint32_t prop_id;
	uint32_t flags;
	char name[DRM_PROP_NAME_LEN];
	int count_values;
	uint64_t *values;
	int count_enums;
	struct drm_mode_property_enum *enums;
	int count_blobs;
	uint32_t *blob_ids;
} drmModePropertyRes, *drmModePropertyPtr;

typedef struct _drmModePropertyBlob {
	uint32_t id;
	uint32_t length;
	void *data;
} drmModePropertyBlobRes, *drmModePropertyBlobPtr;

typedef struct _drmModeObjectProperties {
	uint32_t count_props;
	uint32_t *props;
	uint64_t *prop_values;
} drmModeObjectProperties, *drmModeObjectPropertiesPtr;

typedef struct _drmModePlane {
	uint32_t count_formats;
	uint32_t *formats;
	uint32_t plane_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t crtc_x, crtc_y;
	uint32_t x, y;
	uint32_t possible_crtcs;
	uint32_t gamma_size;
} drmModePlane, *drmModePlanePtr;

typedef struct _drmModePlaneRes {
	uint32_t count_planes;
	uint32_t *planes;
} drmModePlaneRes, *drmModePlaneResPtr;

/* A walk over an IN_FORMATS blob (struct drm_format_modifier_blob). */
typedef struct drmModeFormatModifierIterator {
	uint32_t fmt_idx, mod_idx;
	uint32_t fmt;
	uint64_t mod;
} drmModeFormatModifierIterator;

static inline bool drm_property_type_is(const drmModePropertyPtr property, uint32_t type)
{
	if (property->flags & DRM_MODE_PROP_EXTENDED_TYPE)
		return (property->flags & DRM_MODE_PROP_EXTENDED_TYPE) == type;
	return property->flags & type;
}

static inline uint32_t drmModeGetPropertyType(const drmModePropertyRes *prop)
{
	return prop->flags & (DRM_MODE_PROP_LEGACY_TYPE | DRM_MODE_PROP_EXTENDED_TYPE);
}

drmModeResPtr drmModeGetResources(int fd);
void drmModeFreeResources(drmModeResPtr ptr);
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtc_id);
void drmModeFreeCrtc(drmModeCrtcPtr ptr);
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id);
void drmModeFreeEncoder(drmModeEncoderPtr ptr);
/* Both return the modes of the last detection: a file that is not DRM
 * master never makes the driver probe. */
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connector_id);
drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t connector_id);
void drmModeFreeConnector(drmModeConnectorPtr ptr);
/* "DP-1": the connector's type name and type index, as the kernel names
 * it (NULL for an unknown type). */
const char *drmModeGetConnectorTypeName(uint32_t connector_type);
drmModePlaneResPtr drmModeGetPlaneResources(int fd);
void drmModeFreePlaneResources(drmModePlaneResPtr ptr);
drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id);
void drmModeFreePlane(drmModePlanePtr ptr);
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id,
						      uint32_t object_type);
void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr);
drmModePropertyPtr drmModeGetProperty(int fd, uint32_t property_id);
void drmModeFreeProperty(drmModePropertyPtr ptr);
drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id);
void drmModeFreePropertyBlob(drmModePropertyBlobPtr ptr);
bool drmModeFormatModifierBlobIterNext(const drmModePropertyBlobRes *blob,
				       drmModeFormatModifierIterator *iter);

/* Framebuffers of the file's buffer handles (GEM handles of this primary
 * file: drmPrimeFDToHandle on it). */
int drmModeAddFB2(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
		  const uint32_t bo_handles[4], const uint32_t pitches[4],
		  const uint32_t offsets[4], uint32_t *buf_id, uint32_t flags);
int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
			       const uint32_t bo_handles[4], const uint32_t pitches[4],
			       const uint32_t offsets[4], const uint64_t modifier[4],
			       uint32_t *buf_id, uint32_t flags);
int drmModeRmFB(int fd, uint32_t buffer_id);
int drmModeCloseFB(int fd, uint32_t buffer_id);

#if defined(__cplusplus)
}
#endif
#endif
