/* linuxu: SHIM (third_party/linux/include/drm/drm_privacy_screen_consumer.h) */
#ifndef __DRM_PRIVACY_SCREEN_CONSUMER_H
#define __DRM_PRIVACY_SCREEN_CONSUMER_H

#include <linux/notifier.h>
#include <drm/drm_connector.h>   /* enum drm_privacy_screen_status */

struct drm_privacy_screen;
struct drm_privacy_screen_notifier;

static inline int drm_privacy_screen_register_notifier(struct drm_privacy_screen *priv,
						       struct drm_privacy_screen_notifier *notifier)
{
	(void)priv; (void)notifier;
	return 0;
}

static inline int drm_privacy_screen_unregister_notifier(struct drm_privacy_screen *priv,
							 struct drm_privacy_screen_notifier *notifier)
{
	(void)priv; (void)notifier;
	return 0;
}


static inline int drm_privacy_screen_get_state(struct drm_privacy_screen *priv,
					       enum drm_privacy_screen_status *sw,
					       enum drm_privacy_screen_status *hw)
{
	(void)priv;
	if (sw) *sw = PRIVACY_SCREEN_DISABLED;
	if (hw) *hw = PRIVACY_SCREEN_DISABLED;
	return 0;
}

static inline int drm_privacy_screen_set_sw_state(struct drm_privacy_screen *priv,
						  enum drm_privacy_screen_status sw)
{
	(void)priv; (void)sw;
	return 0;
}

#endif
