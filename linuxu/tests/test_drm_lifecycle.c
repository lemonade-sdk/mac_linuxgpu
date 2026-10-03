/* Host-only smoke test of real upstream DRM core and device lifecycle. */
#include <stdio.h>
#include <assert.h>
#include <linux/device.h>
#include <linux/err.h>
#include <drm/drm_drv.h>

extern int linuxu_module_init_drm_core_init(void);
extern void linuxu_module_exit_drm_core_exit(void);
static unsigned int parent_releases;
static void parent_release(struct device *dev)
{
	(void)dev;
	parent_releases++;
}

#ifndef DRM_TEST_FEATURES
#define DRM_TEST_FEATURES (DRIVER_GEM | DRIVER_RENDER)
#endif

static const struct drm_driver test_driver = {
	.driver_features = DRM_TEST_FEATURES,
	.name = "linuxu-test",
	.desc = "Linux shim lifecycle test",
	.major = 1,
	.minor = 0,
};

int main(void)
{
	struct drm_device *drm;
	struct device parent = { 0 };
	int ret = linuxu_module_init_drm_core_init();

	if (ret) {
		fprintf(stderr, "drm_core_init: %d\n", ret);
		return 1;
	}
	device_initialize(&parent);
	parent.release = parent_release;
	if (dev_set_name(&parent, "linuxu-parent") < 0) {
		put_device(&parent);
		linuxu_module_exit_drm_core_exit();
		return 3;
	}
	/* DRM publishes minor children under an already registered PCI/platform
	 * parent; merely initializing its reference count is not registration. */
	ret = device_add(&parent);
	if (ret) {
		fprintf(stderr, "parent device_add: %d\n", ret);
		put_device(&parent);
		linuxu_module_exit_drm_core_exit();
		return 3;
	}
	assert(device_is_registered(&parent));
	drm = drm_dev_alloc(&test_driver, &parent);
	if (IS_ERR(drm)) {
		fprintf(stderr, "drm_dev_alloc: %ld\n", PTR_ERR(drm));
		int error = (int)-PTR_ERR(drm);
		device_unregister(&parent);
		linuxu_module_exit_drm_core_exit();
		return error < 256 ? error : 2;
	}
	if (!drm->primary ||
	    ((DRM_TEST_FEATURES & DRIVER_RENDER) && !drm->render) ||
	    ((DRM_TEST_FEATURES & DRIVER_GEM) && !drm->vma_offset_manager)) {
		fprintf(stderr, "drm_dev_alloc: missing minor or GEM manager\n");
		drm_dev_put(drm);
		device_unregister(&parent);
		linuxu_module_exit_drm_core_exit();
		return 4;
	}
	ret = drm_dev_register(drm, 0);
	if (ret) {
		fprintf(stderr, "drm_dev_register: %d\n", ret);
		drm_dev_put(drm);
		device_unregister(&parent);
		linuxu_module_exit_drm_core_exit();
		return -ret < 256 ? -ret : 5;
	}
	if (!drm->registered) {
		drm_dev_unregister(drm);
		drm_dev_put(drm);
		device_unregister(&parent);
		linuxu_module_exit_drm_core_exit();
		return 6;
	}
	drm_dev_unregister(drm);
	drm_dev_put(drm);
	assert(!parent_releases);
	device_unregister(&parent);
	assert(parent_releases == 1 && !parent.kobj.sd);
	linuxu_module_exit_drm_core_exit();
	return 0;
}
