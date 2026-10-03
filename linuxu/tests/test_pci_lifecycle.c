/* Mock the one active DriverKit PCI device; no hardware access.
 * Build: clang -w -std=gnu11 -DLINUXU_DEXT_DK=1 -ffunction-sections
 *   -fdata-sections -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include
 *   linuxu/tests/test_pci_lifecycle.c linuxu/src/pci/pci_stub.c
 *   -Wl,-dead_strip -lpthread -o /tmp/test_pci_lifecycle
 *   && /tmp/test_pci_lifecycle
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <linux/pci.h>
#include <rt/rt.h>
#include <rt/ttm_cleanup.h>

static struct pci_dev *active;
static unsigned int probe_calls;
static unsigned int remove_calls;
static int probe_result;
static unsigned int resources_released;
static unsigned int cleanup_calls;
static int cleanup_result;
static int cleanup_finished;
static const struct pci_device_id *expected_id;

#ifdef LINUXU_TEST_RT_DEVICE_FREE
/* The runner extracts these unchanged definitions from production device.c.
 * Every resource-release boundary below is still the real PCI/devres code. */
#include "pci_runtime_free.inc"
void device_unregister(struct device *dev)
{
	(void)dev;
	assert(!"retained runtime device must not reach device_unregister");
}
#endif

/* The driver's dev_groups: added once probe succeeded with the driver bound,
 * removed before its remove callback. */
static const struct attribute_group test_group = { .name = "memory" };
static const struct attribute_group *test_groups[] = { &test_group, NULL };
static int groups_present, groups_error;
static unsigned int groups_added;
int device_add_groups(struct device *dev, const struct attribute_group **groups)
{
	assert(groups == test_groups && !groups_present && dev->driver);
	assert(pci_get_drvdata(to_pci_dev(dev)) == (void *)(uintptr_t)0x1234);
	if (groups_error)
		return groups_error;
	groups_present = 1;
	groups_added++;
	return 0;
}
void device_remove_groups(struct device *dev, const struct attribute_group **groups)
{
	assert(dev && (!groups || groups == test_groups));
	if (groups)
		groups_present = 0;
}

struct pci_dev *rt_device_active_pdev(void) { return active; }
int dext_pci_config_read32(uint64_t offset, uint32_t *value)
{
	if (offset || !active)
		return -1;
	*value = active->vendor | ((uint32_t)active->device << 16);
	return 0;
}

static void release_probe_resource(void *data)
{
	struct pci_dev *dev = data;
	assert(dev->dev.driver != NULL);
	assert(pci_get_drvdata(dev) == (void *)(uintptr_t)0x1234);
	assert(!probe_result || cleanup_finished);
	resources_released++;
}

int rt_amdgpu_cleanup_failed_probe(struct pci_dev *dev)
{
	assert(dev == active && probe_result != 0);
	assert(dev->dev.driver && dev->dev.devres);
	assert(pci_get_drvdata(dev) == (void *)(uintptr_t)0x1234);
	assert(rt_pci_probe_result(dev) == -EINPROGRESS);
	assert(!cleanup_finished);
	cleanup_calls++;
	cleanup_finished = cleanup_result >= 0;
	return cleanup_result;
}

static int test_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	assert(dev == active && id == expected_id);
	assert(dev->dev.driver != NULL);
	assert(dev->dev.devres == NULL);
	cleanup_finished = 0;
	assert(devm_add_action_or_reset(&dev->dev, release_probe_resource, dev) == 0);
	probe_calls++;
	assert(rt_pci_probe_result(dev) == -EINPROGRESS);
	pci_set_drvdata(dev, (void *)(uintptr_t)0x1234);
	assert(dev_get_drvdata(&dev->dev) == (void *)(uintptr_t)0x1234);
	return probe_result;
}

static void test_remove(struct pci_dev *dev)
{
	assert(dev == active);
	assert(dev->dev.driver != NULL);
	assert(pci_get_drvdata(dev) == (void *)(uintptr_t)0x1234);
	assert(!groups_present);
	remove_calls++;
}

int main(void)
{
	struct pci_dev gpu = { .vendor = 0x1002, .device = 0x744c,
		.subsystem_vendor = 0x1002, .subsystem_device = 0x0e3c,
		.class = 0x030000 };
	struct pci_dev other = { .vendor = 0x1002, .device = 0x9999 };
	const struct pci_device_id ids[] = {
		{ 0x1002, 0x744c, 0x9999, PCI_ANY_ID, 0x030000, 0xff0000, 1 },
		{ 0x1002, 0x744c, PCI_ANY_ID, PCI_ANY_ID, 0x030000, 0xff0000, 2 },
		{ 0 },
	};
	const struct pci_device_id wrong_ids[] = {
		{ 0x1002, 0x7552, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
		{ 0 },
	};
	struct pci_driver driver = { .name = "amdgpu", .id_table = ids,
		.probe = test_probe, .remove = test_remove, .dev_groups = test_groups };
	struct pci_driver wrong = { .name = "other", .id_table = wrong_ids,
		.probe = test_probe, .remove = test_remove };

	expected_id = &ids[1];
	assert(pci_match_id(ids, &gpu) == expected_id);
	assert(pci_match_id(ids, &other) == NULL);
	assert(pci_match_id(NULL, &gpu) == NULL);
	assert(pci_register_driver(NULL) == -EINVAL);
	assert(pci_register_driver(&driver) == 0);
	assert(rt_pci_probe_result(&gpu) == -EAGAIN);
	assert(probe_calls == 0); /* registration without a device */
	assert(pci_register_driver(&wrong) == -EBUSY);
	assert(pci_probe(NULL) == -EINVAL);
	assert(pci_probe(&gpu) == -ENODEV);
	active = &gpu;
	gpu.dev.bus = &pci_bus_type;
	assert(dev_is_pci(&gpu.dev));
	assert(to_pci_dev(&gpu.dev) == &gpu);
	assert(pci_devices() == &gpu);
	assert(pci_probe(&other) == -ENODEV);
	probe_result = -EIO;
	assert(pci_probe(&gpu) == -EIO);
	assert(rt_pci_probe_result(&gpu) == -EIO);
	assert(probe_calls == 1 && gpu.dev.driver == NULL);
	assert(pci_get_drvdata(&gpu) == NULL);
	assert(resources_released == 1 && gpu.dev.devres == NULL);
	assert(cleanup_calls == 1 && !rt_pci_probe_cleanup_retained(&gpu));
	assert(!rt_pci_has_retained_probe());
	pci_remove(&gpu);
	assert(remove_calls == 0);
	assert(!groups_added);	/* a failed probe adds no dev_groups */
	probe_result = 0;
	assert(pci_probe(&gpu) == 0);
	assert(rt_pci_probe_result(&gpu) == 0);
	assert(probe_calls == 2 && groups_present && groups_added == 1);
	assert(pci_probe(&gpu) == -EBUSY);
	pci_remove(&gpu);
	assert(remove_calls == 1 && gpu.dev.driver == NULL && !groups_present);
	assert(rt_pci_probe_result(&gpu) == -EAGAIN);
	assert(resources_released == 2 && gpu.dev.devres == NULL);
	assert(pci_get_drvdata(&gpu) == NULL);
	assert(pci_probe(&gpu) == 0);
	pci_unregister_driver(&wrong); /* no effect on owning driver */
	assert(remove_calls == 1);
	pci_unregister_driver(&driver);
	assert(remove_calls == 2 && gpu.dev.driver == NULL && !groups_present);
	assert(resources_released == 3 && gpu.dev.devres == NULL);
	/* Failing dev_groups unbinds the driver: remove runs, devres is
	 * released, and it is not a failed upstream probe needing cleanup. */
	assert(pci_register_driver(&driver) == 0);
	groups_error = -ENOMEM;
	pci_remove(&gpu);
	assert(remove_calls == 3);
	assert(pci_probe(&gpu) == -ENOMEM && rt_pci_probe_result(&gpu) == -ENOMEM);
	assert(remove_calls == 4 && cleanup_calls == 1 && !groups_present);
	assert(gpu.dev.driver == NULL && gpu.dev.devres == NULL && resources_released == 5);
	groups_error = 0;
	pci_unregister_driver(&driver);
	/* The counts below continue from before this case. */
	probe_calls = 3;
	remove_calls = 2;
	resources_released = 3;
	assert(pci_probe(&gpu) == -ENODEV);
	pci_unregister_driver(&driver);
	assert(remove_calls == 2);

	assert(pci_register_driver(&wrong) == 0); /* no ID match */
	assert(probe_calls == 3);
	assert(pci_probe(&gpu) == -ENODEV);
	pci_unregister_driver(&wrong);
	probe_result = -EIO;
	assert(pci_register_driver(&driver) == 0); /* probe error is not registration error */
	assert(probe_calls == 4 && gpu.dev.driver == NULL);
	assert(resources_released == 4 && gpu.dev.devres == NULL);
	pci_unregister_driver(&driver);
	/* A recognized pristine partial TTM cleanup must precede devres release,
	 * and the original probe error remains available to the host lifecycle. */
	cleanup_result = 1;
	assert(pci_register_driver(&driver) == 0);
	assert(rt_pci_probe_result(&gpu) == -EIO);
	assert(cleanup_calls == 3 && probe_calls == 5 && resources_released == 5);
	assert(gpu.dev.driver == NULL && gpu.dev.devres == NULL);
	assert(!rt_pci_probe_cleanup_retained(&gpu));
	pci_unregister_driver(&driver);

	/* Ownership refusal is permanent: probe/remove/unregister/free must not
	 * destroy the devres that owns the partial DRM/TTM device. */
	struct pci_dev *retained_dev = &gpu;
#ifdef LINUXU_TEST_RT_DEVICE_FREE
	struct rt_device retained = { .pdev = gpu };
	rt_active_device = &retained;
	retained_dev = &retained.pdev;
	active = retained_dev;
#endif
	cleanup_result = -EBUSY;
	assert(pci_register_driver(&driver) == 0);
	assert(rt_pci_probe_result(retained_dev) == -EIO);
	assert(rt_pci_probe_cleanup_retained(retained_dev));
	assert(rt_pci_has_retained_probe());
	assert(!rt_pci_probe_cleanup_retained(NULL) && !rt_pci_probe_cleanup_retained(&other));
	assert(probe_calls == 6 && cleanup_calls == 4 && resources_released == 5);
	assert(pci_probe(retained_dev) == -EBUSY && probe_calls == 6);
	void *resources = retained_dev->dev.devres;
	assert(resources && retained_dev->dev.driver == &driver.driver);
	pci_remove(retained_dev);
	pci_unregister_driver(&driver);
	assert(pci_register_driver(&wrong) == -EBUSY);
#ifdef LINUXU_TEST_RT_DEVICE_FREE
	rt_device_free(&retained);
	assert(rt_active_device == &retained);
#endif
	assert(remove_calls == 2 && resources_released == 5);
	assert(retained_dev->dev.devres == resources && retained_dev->dev.driver == &driver.driver);
	assert(pci_get_drvdata(retained_dev) == (void *)(uintptr_t)0x1234);
	assert(rt_pci_probe_result(retained_dev) == -EIO);
	assert(rt_pci_probe_cleanup_retained(retained_dev));
	assert(pci_probe(retained_dev) == -EBUSY && probe_calls == 6);
	active = NULL;
	assert(pci_devices() == NULL);
	return 0;
}
