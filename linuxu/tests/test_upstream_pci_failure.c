/* Real upstream amdgpu probe against a mock DriverKit PCI snapshot.
 * The missing BARs force pci_enable_device to fail after the driver's
 * device-managed DRM allocation. Discovery cleanup also runs against an
 * in-memory binary, including partial initialization. No hardware is accessed. */
#include <assert.h>

/* Probe/cleanup simulation has no endpoint capable of a function reset. */
int dext_pci_function_reset(void) { return -95; }
/* The device never leaves the bus in these scenarios. */
int dext_pci_removed(void) { return 0; }
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_memory_sysctl.h"

#include <linux/pci.h>
#include <linux/err.h>
#include <drm/drm_drv.h>
#include <rt/bootstrap.h>
#include <rt/dext_dma.h>
#include <rt/dext_pci.h>
#include <rt/rt.h>
#include <linux/slab.h>
#include "dext_heap_backend.h"

/* Include the unchanged upstream translation unit to reach its private sysfs
 * constructor. The test archive omits its normal discovery object. Only these
 * allocation/registration seams are instrumented; teardown is upstream code. */
static void *discovery_kzalloc(size_t size);
static void discovery_kfree(const void *ptr);
static int discovery_kset_register(struct kset *kset);
#define kzalloc_obj1 discovery_kzalloc
#define kzalloc_flex1 discovery_kzalloc
#define kfree discovery_kfree
#define kset_register discovery_kset_register
#include "../../third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c"
#undef kset_register
#undef kfree
#undef kzalloc_flex1
#undef kzalloc_obj1

static int discovery_tracking;
static int legacy_kset;
static unsigned int discovery_attempts, discovery_fail_at;
static unsigned int discovery_live, discovery_freed;
static unsigned int discovery_parent_releases;
static void *discovery_allocations[64];
static void discovery_parent_release(struct device *dev)
{
	(void)dev;
	discovery_parent_releases++;
}

static void *discovery_kzalloc(size_t size)
{
	void *ptr;
	if (discovery_tracking && ++discovery_attempts == discovery_fail_at)
		return NULL;
	ptr = kzalloc(size, GFP_KERNEL);
	if (discovery_tracking && ptr) {
		unsigned int i;
		for (i = 0; i < ARRAY_SIZE(discovery_allocations); i++)
			if (!discovery_allocations[i])
				break;
		assert(i < ARRAY_SIZE(discovery_allocations));
		discovery_allocations[i] = ptr;
		discovery_live++;
	}
	return ptr;
}

static void discovery_kfree(const void *ptr)
{
	if (discovery_tracking && ptr) {
		for (unsigned int i = 0; i < ARRAY_SIZE(discovery_allocations); i++) {
			if (discovery_allocations[i] != ptr)
				continue;
			discovery_allocations[i] = NULL;
			discovery_live--;
			discovery_freed++;
			break;
		}
	}
	kfree(ptr);
}

static int discovery_kset_register(struct kset *kset)
{
	/* Negative control reproduces build 211's successful no-op registration. */
	return legacy_kset ? 0 : kset_register(kset);
}

static void *discovery_fixture(unsigned int dies, unsigned int ips)
{
	const size_t ip_size = sizeof(struct ip_v4) + sizeof(uint32_t);
	const size_t table_offset = sizeof(struct binary_header);
	const size_t die_size = sizeof(struct die_header) + ips * ip_size;
	const size_t size = table_offset + sizeof(struct ip_discovery_header) +
		dies * die_size;
	uint8_t *bin = kzalloc(size, GFP_KERNEL);
	struct binary_header *binary = (void *)bin;
	struct ip_discovery_header *header = (void *)(bin + table_offset);

	assert(bin);
	binary->binary_signature = cpu_to_le32(BINARY_SIGNATURE);
	binary->version_major = cpu_to_le16(1);
	binary->binary_size = cpu_to_le16(size);
	binary->table_list[IP_DISCOVERY].offset = cpu_to_le16(table_offset);
	header->signature = cpu_to_le32(DISCOVERY_TABLE_SIGNATURE);
	header->version = cpu_to_le16(4);
	header->size = cpu_to_le16(size - table_offset);
	header->num_dies = cpu_to_le16(dies);
	for (unsigned int d = 0; d < dies; d++) {
		size_t offset = table_offset + sizeof(*header) + d * die_size;
		struct die_header *die = (void *)(bin + offset);
		header->die_info[d].die_id = cpu_to_le16(d);
		header->die_info[d].die_offset = cpu_to_le16(offset);
		die->die_id = cpu_to_le16(d);
		die->num_ips = cpu_to_le16(ips);
		for (unsigned int i = 0; i < ips; i++) {
			struct ip_v4 *ip = (void *)((uint8_t *)(die + 1) + i * ip_size);
			ip->hw_id = cpu_to_le16(i < 2 ? GC_HWID : SDMA0_HWID);
			ip->instance_number = i < 2 ? i : 0;
			ip->num_base_address = 1;
			ip->major = 12;
			ip->base_address[0] = cpu_to_le32(0x1000 + i * 0x100);
		}
	}
	return bin;
}

static void check_discovery_tree(struct amdgpu_device *adev,
				 unsigned int dies, unsigned int ips)
{
	struct ip_discovery_top *top = adev->discovery.ip_top;
	struct list_head *d, *h, *i;
	unsigned int die_count = 0, hw_count = 0, ip_count = 0;
	assert(top && !strcmp(kobject_name(&top->kobj), "ip_discovery"));
	assert(top->die_kset.kobj.parent == &top->kobj);
	list_for_each(d, &top->die_kset.list) {
		struct ip_die_entry *die = to_ip_die_entry(list_to_kobj(d));
		assert(die->ip_kset.kobj.parent == &top->die_kset.kobj);
		die_count++;
		list_for_each(h, &die->ip_kset.list) {
			struct ip_hw_id *hw = to_ip_hw_id(list_to_kobj(h));
			assert(hw->hw_id_kset.kobj.parent == &die->ip_kset.kobj);
			hw_count++;
			list_for_each(i, &hw->hw_id_kset.list) {
				struct ip_hw_instance *instance = to_ip_hw_instance(list_to_kobj(i));
				assert(instance->kobj.parent == &hw->hw_id_kset.kobj);
				assert(instance->num_base_addresses == 1);
				ip_count++;
			}
		}
	}
	assert(die_count == dies);
	assert(hw_count == dies * (ips ? 2 : 0));
	assert(ip_count == dies * ips);
}

static void check_discovery_cleanup(unsigned int dies, unsigned int ips,
				    unsigned int fail_at)
{
	struct amdgpu_device *adev = calloc(1, sizeof(*adev));
	struct device dev = {0};
	unsigned int expected_objects = 1 + dies + (ips ? dies * (2 + ips) : 0);
	int result;
	assert(adev && discovery_live == 0);
	device_initialize(&dev);
	dev.release = discovery_parent_release;
	const unsigned int parent_releases_before = discovery_parent_releases;
	assert(dev_set_name(&dev, "discovery-parent") == 0);
	/* Match the registered PCI parent used by the actual upstream probe. */
	assert(device_add(&dev) == 0);
	adev->dev = &dev;
	adev->discovery.bin = discovery_fixture(dies, ips);
	discovery_attempts = discovery_freed = 0;
	discovery_fail_at = fail_at;
	discovery_tracking = 1;
	result = amdgpu_discovery_sysfs_init(adev);
	if (!fail_at && !legacy_kset) {
		assert(result == 0);
		assert(discovery_live == expected_objects);
		check_discovery_tree(adev, dies, ips);
	} else if (fail_at) {
		assert(discovery_attempts >= fail_at);
		/* Upstream ignores an IP-instance constructor error; either result
		 * still requires safe cleanup of the registered partial tree. */
		assert(result == 0 || result == -ENOMEM);
	}
	amdgpu_discovery_fini(adev);
	assert(!adev->discovery.ip_top && !adev->discovery.bin);
	assert(discovery_live == 0);
	if (!fail_at)
		assert(discovery_freed == expected_objects);
	/* Failed probe and subsequent device cleanup must tolerate another fini. */
	amdgpu_discovery_fini(adev);
	discovery_tracking = 0;
	device_unregister(&dev);
	assert(discovery_parent_releases == parent_releases_before + 1 && !dev.kobj.sd);
	free(adev);
}

static void check_all_discovery_cleanup(void)
{
	check_discovery_cleanup(0, 0, 0);
	check_discovery_cleanup(1, 0, 0);
	check_discovery_cleanup(2, 3, 0);
	/* Thirteen upstream object allocations in the complete two-die tree. */
	for (unsigned int fail_at = 1; fail_at <= 13; fail_at++)
		check_discovery_cleanup(2, 3, fail_at);
	puts("upstream discovery cleanup passed (empty, populated, 13 allocation failures)");
}

static unsigned int snapshots;
static unsigned int config_reads;
static unsigned int drm_alloc_calls;
static unsigned int drm_alloc_successes;
static int bars_present;
static uint16_t pci_command;
static unsigned int bar_info_calls;

extern void *__devm_drm_dev_alloc_impl(struct device *parent,
				const struct drm_driver *driver,
				size_t size, size_t offset);
void *__devm_drm_dev_alloc(struct device *parent,
			   const struct drm_driver *driver,
			   size_t size, size_t offset)
{
	void *result = __devm_drm_dev_alloc_impl(parent, driver, size, offset);
	drm_alloc_calls++;
	if (!IS_ERR(result))
		drm_alloc_successes++;
	return result;
}

int dext_pci_snapshot(struct dext_pci_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->vendor = 0x1002;
	snapshot->device = 0x744c;
	snapshot->subsystem_vendor = 0x1002;
	snapshot->subsystem_device = 0x0e3c;
	snapshot->class_code = 0x030000;
	snapshot->bus = 4;
	snapshot->slot = 0;
	if (bars_present) {
		snapshot->bar[0] = (struct dext_pci_bar) {
			.base = 0x40000000000ULL, .size = 16ULL << 30,
			.present = 1,
		};
		snapshot->bar[5] = (struct dext_pci_bar) {
			/* BAR5 must not overlap the mock 16 GiB BAR0 aperture. */
			.base = 0x40410000000ULL, .size = 512UL << 10,
			.present = 1,
		};
	}
	snapshots++;
	return 0;
}

int dext_pci_config_read32(uint64_t offset, uint32_t *value)
{
	if (offset != 0)
		return -1;
	config_reads++;
	*value = 0x744c1002;
	return 0;
}

int dext_pci_config_read16(uint64_t offset, uint16_t *value)
{
	*value = offset == PCI_COMMAND ? pci_command : 0;
	return 0;
}
int dext_pci_config_read8(uint64_t offset, uint8_t *value)
{
	(void)offset;
	*value = 0;
	return 0;
}
/* No MSI-X capability in this configuration: the table is never read. */
int dext_pci_bar_read32(unsigned int bar, uint64_t offset, uint32_t *value)
{
	(void)bar; (void)offset;
	*value = UINT32_MAX;
	return -1;
}
int dext_pci_bar_write32(unsigned int bar, uint64_t offset, uint32_t value)
{
	(void)bar; (void)offset; (void)value;
	return -1;
}
int dext_pci_config_write32(uint64_t offset, uint32_t value)
{
	(void)offset; (void)value;
	return 0;
}
int dext_pci_config_write16(uint64_t offset, uint16_t value)
{
	if (offset == PCI_COMMAND)
		pci_command = value;
	return 0;
}
int dext_pci_config_write8(uint64_t offset, uint8_t value)
{
	(void)offset; (void)value;
	return 0;
}
int dext_pci_irq_status(unsigned int *armed, unsigned int *type)
{
	*armed = 0;
	*type = DEXT_PCI_IRQ_NONE;
	return 0;
}
int dext_bar_info(uint8_t bar, uint8_t *index, uint64_t *size)
{
	(void)bar; (void)index; (void)size;
	bar_info_calls++;
	return -1;
}
void *dext_bar0_cpu_map(uint64_t offset, uint64_t size)
{
	(void)offset; (void)size;
	/* These failure cases must stop before mapping VRAM. */
	abort();
}
int dext_bar0_cpu_unmap(const void *address)
{
	(void)address;
	return 0;
}
int dext_bar0_cpu_contains(const void *address, size_t size)
{
	(void)address; (void)size;
	return 0;
}
int dext_mem_read8(uint32_t token, uint64_t offset, uint8_t *value)
{
	(void)token; (void)offset; (void)value;
	abort();
}
int dext_mem_write8(uint32_t token, uint64_t offset, uint8_t value)
{
	(void)token; (void)offset; (void)value;
	abort();
}
void dext_pci_transport_record_fault(int fault, uint64_t offset)
{
	(void)fault; (void)offset;
	abort();
}

int main(int argc, char **argv)
{
	struct rt_device *device;
	struct pci_dev *pdev;
	if (argc == 3 && strcmp(argv[1], "--probe-oom") == 0) {
		dext_heap_test_enable_abort_trace();
		long fail_after = strtol(argv[2], NULL, 10);
		assert(fail_after >= 0);
		bars_present = 1;
		device = rt_device_alloc();
		assert(device);
		pdev = rt_device_get_pdev(device);
		dext_heap_test_fail_after(fail_after);
		int result = linuxu_driver_bootstrap();
		dext_heap_test_fail_after(-1);
		assert(result <= 0);
		if (!result) {
			assert(rt_pci_probe_result(pdev) < 0);
			assert(!pdev->dev.driver && !pci_get_drvdata(pdev));
			assert(!pdev->dev.devres && pdev->enable_cnt == 0);
		}
		linuxu_driver_shutdown();
		linuxu_driver_shutdown();
		rt_device_free(device);
		assert(rt_device_active_pdev() == NULL);
		printf("production heap upstream probe OOM %ld passed\n", fail_after);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--discovery-legacy-kset") == 0) {
		legacy_kset = 1;
		check_discovery_cleanup(0, 0, 0);
		return 0;
	}
	check_all_discovery_cleanup();

	device = rt_device_alloc();
	assert(device);
	pdev = rt_device_get_pdev(device);
	assert(pdev && pdev == rt_device_active_pdev());
	assert(pdev->vendor == 0x1002 && pdev->device == 0x744c);
	assert(pdev->class == 0x030000);
	assert(!pdev->resource[0].flags && !pdev->resource[5].flags);
	assert(rt_pci_probe_result(pdev) == -EAGAIN);
	assert(linuxu_driver_bootstrap() == 0);
	assert(config_reads > 0);
	assert(drm_alloc_calls == 1 && drm_alloc_successes == 1);
	assert(rt_pci_probe_result(pdev) == -ENODEV);
	assert(!pdev->dev.driver && !pci_get_drvdata(pdev));
	assert(!pdev->dev.devres && pdev->enable_cnt == 0);
	linuxu_driver_shutdown();
	assert(rt_pci_probe_result(pdev) == -EAGAIN);
	assert(linuxu_driver_bootstrap() == 0);
	assert(drm_alloc_calls == 2 && drm_alloc_successes == 2);
	assert(rt_pci_probe_result(pdev) == -ENODEV);
	assert(!pdev->dev.driver && !pdev->dev.devres);
	linuxu_driver_shutdown();
	rt_device_free(device);
	assert(rt_device_active_pdev() == NULL);
	assert(snapshots == 1);
	if (argc > 1 && strcmp(argv[1], "--bars") == 0) {
		bars_present = 1;
		device = rt_device_alloc();
		assert(device);
		pdev = rt_device_get_pdev(device);
		assert(pdev->resource[0].flags && pdev->resource[5].flags);
		assert(linuxu_driver_bootstrap() == 0);
		assert(rt_pci_probe_result(pdev) == -ENOMEM);
		assert(drm_alloc_calls == 3 && drm_alloc_successes == 3);
		assert(bar_info_calls > 0);
		assert(!pdev->dev.driver && !pci_get_drvdata(pdev));
		assert(!pdev->dev.devres && pdev->enable_cnt == 0);
		assert((pci_command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) == 0);
		linuxu_driver_shutdown();
		rt_device_free(device);
	}
	puts("upstream PCI failure unwind passed");
	return 0;
}
