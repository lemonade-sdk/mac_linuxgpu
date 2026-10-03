/* linuxu: SHIM (display surface — drm_connector_list_iter + the
 * connector/encoder/plane init helpers amdgpu_mode.c calls heavily;
 * no modeset in the compute dext, these resolve at link time) */
#ifndef _DRM_CONNECTOR_LIST_ITER_LINUXU_H
#define _DRM_CONNECTOR_LIST_ITER_LINUXU_H

#include <drm/drm_device.h>
#include <drm/drm_connector.h>

/**
 * struct drm_connector_list_iter - connector_list iterator
 */


void drm_connector_list_iter_begin(struct drm_device *dev,
				   struct drm_connector_list_iter *iter);
void drm_connector_list_iter_end(struct drm_connector_list_iter *iter);
struct drm_connector *
drm_connector_list_iter_next(struct drm_connector_list_iter *iter);

/**
 * drm_for_each_connector - iterate over all connectors
 */
#define drm_for_each_connector(connector, dev)		\
	list_for_each_entry((connector), &(dev)->mode_config.connector_list, \
			     head)

/**
 * drm_for_each_connector_iter - iterate over all connectors (iterator form)
 */
#define drm_for_each_connector_iter(connector, iter)	\
	for ((connector) = drm_connector_list_iter_next(iter); \
	     (connector); \
	     (connector) = drm_connector_list_iter_next(iter))

/**
 * drm_for_each_connector_ret - iterate over all connectors with break
 */
#define drm_for_each_connector_ret(connector, dev, __ret)	\
	list_for_each_entry((connector), &(dev)->mode_config.connector_list, \
			     head)

#endif
