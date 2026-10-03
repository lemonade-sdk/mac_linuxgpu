/* Exercise platform registration and nested devres ownership without IOKit.
 * Build: clang -w -std=gnu11 -D__KERNEL__ -ffunction-sections
 *   -fdata-sections -Ilinuxu/headers linuxu/tests/test_platform_devres.c
 *   linuxu/src/kmem/slab.c linuxu/src/shims/platform_device.c
 *   -Wl,-dead_strip -o /tmp/test_platform_devres
 *   && /tmp/test_platform_devres
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>

static int device_add_count;
static int device_release_count;
static int actions[8];
static int action_count;
void linuxu_warn(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; abort(); }

void device_initialize(struct device *dev) { refcount_set(&dev->kobj.kref, 1); }
int device_add(struct device *dev) { (void)dev; device_add_count++; return 0; }
void put_device(struct device *dev)
{
	if (!refcount_dec_and_test(&dev->kobj.kref)) return;
	const char *name = dev->kobj.name_owned ? dev->kobj.name : NULL;
	devres_release_all(dev);
	dev->release(dev);
	free((void *)name);
	device_release_count++;
}
void device_unregister(struct device *dev) { put_device(dev); }
int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (vasprintf((char **)&kobj->name, fmt, ap) < 0) {
		va_end(ap);
		return -ENOMEM;
	}
	va_end(ap);
	kobj->name_owned = true;
	return 0;
}
void *kmalloc(size_t size, gfp_t flags) { (void)flags; return malloc(size); }
void *kzalloc(size_t size, gfp_t flags) { (void)flags; return calloc(1, size); }
void kfree(const void *p) { free((void *)p); }

static irqreturn_t unexpected_irq_handler(int irq, void *data)
{
	(void)irq; (void)data;
	abort();
}

static void record_action(void *opaque)
{
	actions[action_count++] = (int)(uintptr_t)opaque;
}

int main(void)
{
	struct platform_device *pdev;
	void *outer, *inner;

	pdev = platform_device_register_simple("amdgpu_xcp_0", -1, NULL, 0);
	assert(!IS_ERR(pdev));
	assert(strcmp(pdev->name, "amdgpu_xcp_0") == 0);
	assert(device_add_count == 1);
	/* An unsupported managed request cannot advertise an enabled interrupt
	 * or silently install a devres ownership record. */
	assert(!pdev->dev.devres);
	assert(devm_request_irq(&pdev->dev, 7, unexpected_irq_handler, 0,
				"offline", pdev) == -EOPNOTSUPP);
	assert(devm_request_threaded_irq(&pdev->dev, 7, unexpected_irq_handler,
				unexpected_irq_handler, IRQF_ONESHOT,
				"offline", pdev) == -EOPNOTSUPP);
	devm_free_irq(&pdev->dev, 7, pdev);
	assert(!pdev->dev.devres);
	assert(platform_device_register_simple(NULL, -1, NULL, 0) ==
	       ERR_PTR(-EINVAL));
	assert(platform_device_register_simple("bad", -1, NULL, 1) ==
	       ERR_PTR(-EINVAL));

	outer = devres_open_group(&pdev->dev, NULL, 0);
	assert(outer);
	assert(devm_add_action_or_reset(&pdev->dev, record_action,
					 (void *)(uintptr_t)1) == 0);
	inner = devres_open_group(&pdev->dev, NULL, 0);
	assert(inner && inner != outer);
	assert(devm_add_action_or_reset(&pdev->dev, record_action,
					 (void *)(uintptr_t)2) == 0);
	devres_close_group(&pdev->dev, inner);
	assert(devm_add_action_or_reset(&pdev->dev, record_action,
					 (void *)(uintptr_t)3) == 0);
	assert(devres_release_group(&pdev->dev, inner) == 1);
	assert(action_count == 1 && actions[0] == 2);
	assert(devres_release_group(&pdev->dev, inner) == -ENOENT);
	assert(devres_release_group(&pdev->dev, outer) == 2);
	assert(action_count == 3 && actions[1] == 3 && actions[2] == 1);
	assert(devres_release_group(&pdev->dev, NULL) == -ENOENT);
	assert(devm_add_action_or_reset(&pdev->dev, record_action,
					 (void *)(uintptr_t)4) == 0);
	refcount_inc(&pdev->dev.kobj.kref);
	platform_device_unregister(pdev);
	assert(action_count == 3 && device_release_count == 0);
	assert(pdev->dev.devres != NULL);
	put_device(&pdev->dev);
	assert(action_count == 4 && actions[3] == 4);
	assert(device_release_count == 1);

	/* Release an open, unnamed group exactly as the XCP driver does. */
	pdev = platform_device_register_simple("amdgpu_xcp_1", 7, NULL, 0);
	assert(!IS_ERR(pdev));
	assert(strcmp(pdev->name, "amdgpu_xcp_1.7") == 0);
	assert(devres_open_group(&pdev->dev, NULL, 0));
	assert(devm_add_action_or_reset(&pdev->dev, record_action,
					 (void *)(uintptr_t)5) == 0);
	assert(devres_release_group(&pdev->dev, NULL) == 1);
	platform_device_unregister(pdev);
	assert(action_count == 5 && actions[4] == 5);
	assert(device_release_count == 2);
	return 0;
}
