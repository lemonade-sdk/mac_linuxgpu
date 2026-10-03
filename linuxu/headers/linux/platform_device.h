/* linuxu: SHIM (third_party/linux/include/linux/platform_device.h) */
#ifndef __LINUX_PLATFORM_DEVICE_H
#define __LINUX_PLATFORM_DEVICE_H
#include <linux/device.h>
struct resource;
struct platform_device {
	struct device dev;
	const char *name;
	int id;
	struct resource *resource;
	unsigned int num_resources;
};
extern struct platform_device *platform_device_register_simple(
		const char *name, int id, const struct resource *resources,
		unsigned int num);
extern void platform_device_unregister(struct platform_device *pdev);
#endif
