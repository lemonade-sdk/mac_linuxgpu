/* linuxu: SHIM (display surface — crtc helper entry points
 * amdgpu_mode.c / atombios_crtc.c call; no modeset in the compute dext) */
#ifndef _DRM_CRTC_HELPER_H_
#define _DRM_CRTC_HELPER_H_

#include <drm/drm_crtc.h>
#include <drm/drm_modes.h>

struct drm_device;
struct drm_crtc;
struct drm_plane;
struct drm_encoder;
struct drm_atomic_state;

int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
		  const struct drm_crtc_funcs *funcs);
void drm_crtc_helper_add(struct drm_crtc *crtc,
			 const struct drm_crtc_helper_funcs *funcs);
void drm_crtc_cleanup(struct drm_crtc *crtc);
int drm_crtc_register_of_property(struct drm_crtc *crtc);
void drm_crtc_init_with_mask(struct drm_device *dev, struct drm_crtc *crtc,
			     const struct drm_crtc_helper_funcs *funcs,
			     u32 possible_clones);

void drm_crtc_init_vblank(struct drm_crtc *crtc);






















/* Mode set */
int drm_crtc_set_mode(struct drm_crtc *crtc, struct drm_display_mode *mode,
		      int x, int y);
void drm_crtc_helper_set_mode(struct drm_crtc *crtc,
			      const struct drm_display_mode *mode,
			      int x, int y,
			      struct drm_framebuffer *old_fb);
int drm_crtc_helper_set_config(struct drm_mode_set *set,
			       struct drm_modeset_acquire_ctx *ctx);
struct drm_atomic_state;
struct drm_modeset_acquire_ctx;
static inline int drm_crtc_helper_set_config_atomic(struct drm_device *dev,
						    struct drm_atomic_state *state,
						    struct drm_modeset_acquire_ctx *ctx)
{
	(void)dev; (void)state; (void)ctx;
	return 0;
}

void drm_crtc_helper_set_mode_no_ctx(struct drm_crtc *crtc,
				     const struct drm_display_mode *mode,
				     const struct drm_display_mode *adjusted_mode,
				     int x, int y,
				     struct drm_framebuffer *old_fb);
void drm_crtc_helper_disable(struct drm_crtc *crtc);
void drm_crtc_helper_disable_atomic(struct drm_crtc *crtc,
				    struct drm_atomic_state *state);
void drm_crtc_helper_enable(struct drm_crtc *crtc);
int drm_crtc_helper_prepare(struct drm_crtc *crtc);
void drm_crtc_helper_cleanup(struct drm_crtc *crtc);
int drm_crtc_helper_page_flip(struct drm_crtc *crtc,
			      struct drm_framebuffer *fb,
			      struct drm_pending_vblank_event *event,
			      unsigned int flags);
void drm_crtc_helper_page_flip_work(struct drm_crtc *crtc,
				    struct drm_pending_vblank_event *event,
				    unsigned int flags);

void drm_crtc_helper_hotplug_conn(struct drm_device *dev,
				  struct drm_connector *connector);
void drm_crtc_helper_hotplug_encoded_connectors(struct drm_crtc *crtc);
void drm_crtc_helper_hotplug(struct drm_crtc *crtc);
void drm_crtc_helper_atomic_commit(struct drm_crtc *crtc,
				   struct drm_atomic_state *state);
void drm_crtc_helper_atomic_prepare(struct drm_crtc *crtc,
				    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state);
void drm_crtc_helper_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_update(struct drm_crtc *crtc,
				   struct drm_atomic_state *state);
void drm_crtc_helper_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_tail(struct drm_crtc *crtc,
					struct drm_atomic_state *state);
void drm_crtc_helper_atomic_wait_for_commit(struct drm_crtc *crtc,
					    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_wait_for_vblank(struct drm_crtc *crtc,
					    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_wait_for_vblanks(struct drm_crtc *crtc,
					      struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_hw_done(struct drm_crtc *crtc,
					   struct drm_atomic_state *state);
void drm_crtc_helper_atomic_wait_for_fences(struct drm_crtc *crtc,
					    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_planes(struct drm_crtc *crtc,
					  struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_cleanup_job(struct drm_crtc *crtc,
						struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_tail_rpm(struct drm_crtc *crtc,
					    struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_prepare(struct drm_crtc *crtc,
					   struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_prepare_planes(struct drm_crtc *crtc,
						  struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_prepare_crtcs(struct drm_crtc *crtc,
						 struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_prepare_connectors(struct drm_crtc *crtc,
						      struct drm_atomic_state *state);
void drm_crtc_helper_atomic_commit_prepare_encoders(struct drm_crtc *crtc,
						    struct drm_atomic_state *state);




static inline u32 drm_helper_connector_detect(struct drm_connector *connector,
						 unsigned int min_height,
						 unsigned int min_width)
{
	(void)connector; (void)min_height; (void)min_width;
	return 0;
}

static inline void drm_helper_resume_force_mode(struct drm_device *dev)
{
	(void)dev;
}
#endif
extern void drm_helper_force_disable_all(struct drm_device *dev);
