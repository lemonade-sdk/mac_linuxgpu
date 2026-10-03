#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kdev_t.h>
#include <linux/sysfs.h>

/* Focus the test on the class/device lifecycle without the full runtime. */
const char linuxu_dma_ops = 0;
static int fail_allocation;
static int devres_releases;
void *kzalloc(size_t size, gfp_t flags)
{
	(void)flags;
	if (fail_allocation) {
		fail_allocation = 0;
		return NULL;
	}
	return calloc(1, size);
}
void kfree(const void *p) { free((void *)p); }
void spin_lock_init(spinlock_t *lock) { (void)lock; }
void devres_release_all(struct device *dev)
{
	/* Count released payloads, including idempotent cleanup at final put. */
	if (dev->devres) { dev->devres = NULL; devres_releases++; }
}
void linuxu_bug(const char *file, int line)
{
	(void)file;
	(void)line;
	abort();
}
void linuxu_warn(const char *file, int line, const char *fmt, ...)
{
	(void)file;
	(void)line;
	(void)fmt;
}

int main(void)
{
	struct class *drm;
	struct class kfd = { .name = "kfd" };
	struct class same_name = { .name = "kfd" };
	struct device *dev;
	struct device *second;
	struct attribute version = { .name = "version", .mode = 0444 };
	struct attribute duplicate_version = { .name = "version", .mode = 0444 };
	dev_t number = MKDEV(254, 0);
	put_device(NULL);

	fail_allocation = 1;
	assert(PTR_ERR(class_create("drm")) == -ENOMEM);
	drm = class_create("drm");
	assert(!IS_ERR_OR_NULL(drm));
	assert(class_create_file(drm, &version) == 0);
	assert(class_create_file(drm, &version) == -EEXIST);
	assert(class_create_file(drm, &duplicate_version) == -EEXIST);
	class_remove_file(drm, &version);
	assert(class_create_file(drm, &version) == 0);
	class_destroy(drm);

	assert(class_register(&kfd) == 0);
	assert(class_register(&same_name) == -EEXIST);
	fail_allocation = 1;
	assert(PTR_ERR(device_create(&kfd, NULL, number, NULL, "kfd")) == -ENOMEM);
	dev = device_create(&kfd, NULL, number, NULL, "kfd%d", 7);
	assert(!IS_ERR_OR_NULL(dev));
	assert(dev->devt == number);
	assert(strcmp(dev_name(dev), "kfd7") == 0);
	dev->devres = (void *)1;
	second = device_create(&kfd, NULL, number, NULL, "duplicate");
	assert(PTR_ERR(second) == -EEXIST);
	get_device(dev);
	device_destroy(&kfd, number);
	assert(devres_releases == 0 && dev->devres && !device_is_registered(dev));
	put_device(dev);
	assert(devres_releases == 1);
	dev = device_create(&kfd, NULL, number, NULL, "kfd");
	assert(!IS_ERR_OR_NULL(dev));
	dev->devres = (void *)1;
	get_device(dev);
	class_unregister(&kfd); /* also tears down a still registered device */
	assert(devres_releases == 1 && dev->devres && !device_is_registered(dev));
	put_device(dev);
	assert(devres_releases == 2);
	assert(PTR_ERR(device_create(&kfd, NULL, number, NULL, "kfd")) == -ENODEV);
	assert(class_create_file(&kfd, &version) == -EINVAL);
	return 0;
}
