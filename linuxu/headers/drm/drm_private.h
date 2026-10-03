/* linuxu: SHIM (third_party/linux/include/drm/drm_private.h) */
#ifndef __DRM_PRIVATE_H__
#define __DRM_PRIVATE_H__

#include <drm/drm_mode_object.h>

struct drm_private_state;

struct drm_private_state_funcs {
	void (*destroy)(void *obj_state);
};

struct drm_private_state {
	struct drm_mode_object *obj;
	void *private;
	const struct drm_private_state_funcs *funcs;
};

#define DRM_PRIVATE_STATE(obj, __type, __member) \
	{ \
		.obj = (obj), \
		.private = &(obj)->__member, \
	}

static inline void *drm_private_get(struct drm_private_state *s)
{
	return s->private;
}
static inline void drm_private_set(struct drm_private_state *s, void *priv)
{
	s->private = priv;
}

#endif /* __DRM_PRIVATE_H__ */
