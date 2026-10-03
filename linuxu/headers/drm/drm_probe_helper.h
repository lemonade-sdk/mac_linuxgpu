/* linuxu: SHIM (display surface — drm_probe_helper poll/HPD entry
 * points amdgpu_display.c calls; no modeset in the compute dext) */
#ifndef __DRM_PROBE_HELPER_H__
#define __DRM_PROBE_HELPER_H__

#include <drm/drm_modes.h>

struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_modeset_acquire_ctx;

static inline unsigned int drm_helper_probe_single_connector_modes(struct drm_connector
						    *connector, uint32_t maxX,
						    uint32_t maxY)
{
	(void)connector; (void)maxX; (void)maxY;
	return 0;
}
int drm_helper_probe_detect(struct drm_connector *connector,
			    struct drm_modeset_acquire_ctx *ctx,
			    bool force);

void drmm_kms_helper_poll_init(struct drm_device *dev);
void drm_kms_helper_poll_init(struct drm_device *dev);
void drm_kms_helper_poll_fini(struct drm_device *dev);
bool drm_helper_hpd_irq_event(struct drm_device *dev);
bool drm_connector_helper_hpd_irq_event(struct drm_connector *connector);
void drm_kms_helper_hotplug_event(struct drm_device *dev);
void drm_kms_helper_connector_hotplug_event(struct drm_connector *connector);

void drm_kms_helper_poll_disable(struct drm_device *dev);
void drm_kms_helper_poll_enable(struct drm_device *dev);
void drm_kms_helper_poll_reschedule(struct drm_device *dev);
bool drm_kms_helper_is_poll_worker(void);

enum drm_mode_status drm_crtc_helper_mode_valid_fixed(struct drm_crtc *crtc,
						      const struct drm_display_mode *mode,
						      const struct drm_display_mode *fixed_mode);

int drm_connector_helper_get_modes_fixed(struct drm_connector *connector,
					 const struct drm_display_mode *fixed_mode);
int drm_connector_helper_get_modes(struct drm_connector *connector);
int drm_connector_helper_tv_get_modes(struct drm_connector *connector);

int drm_connector_helper_detect_from_ddc(struct drm_connector *connector,
					 struct drm_modeset_acquire_ctx *ctx,
					 bool force);

#endif
