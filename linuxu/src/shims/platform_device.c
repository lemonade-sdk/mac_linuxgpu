/* In-memory platform devices for auxiliary DRM/XCP instances. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/err.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

static void platform_device_release(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						       dev);

	free(pdev->resource);
	/* kobject_put owns the name and releases it after this callback. */
	free(pdev);
}

struct platform_device *platform_device_register_simple(
		const char *name, int id, const struct resource *resources,
		unsigned int num)
{
	struct platform_device *pdev;
	int ret;

	if (!name || !*name || (num && !resources) ||
	    num > SIZE_MAX / sizeof(*resources))
		return ERR_PTR(-EINVAL);
	pdev = calloc(1, sizeof(*pdev));
	if (!pdev)
		return ERR_PTR(-ENOMEM);
	if (num) {
		pdev->resource = malloc((size_t)num * sizeof(*resources));
		if (!pdev->resource) {
			free(pdev);
			return ERR_PTR(-ENOMEM);
		}
		memcpy(pdev->resource, resources,
		       (size_t)num * sizeof(*resources));
	}
	pdev->id = id;
	pdev->num_resources = num;
	pdev->dev.release = platform_device_release;
	device_initialize(&pdev->dev);
	ret = id == -1 ? kobject_set_name(&pdev->dev.kobj, "%s", name) :
		kobject_set_name(&pdev->dev.kobj, "%s.%d", name, id);
	if (!ret)
		ret = device_add(&pdev->dev);
	if (ret) {
		put_device(&pdev->dev);
		return ERR_PTR(ret);
	}
	pdev->name = pdev->dev.kobj.name;
	pdev->dev.init_name = pdev->name;
	return pdev;
}

void platform_device_unregister(struct platform_device *pdev)
{
	if (!pdev || IS_ERR(pdev))
		return;
	device_unregister(&pdev->dev);
}
