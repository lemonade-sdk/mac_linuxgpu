/* Production platform/kobject/devres teardown over the DriverKit heap adapter.
 * All allocations are simulated; this process never connects to IOKit. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include "dext_heap_backend.h"

/* No DMA operation is reachable in these platform lifecycle paths. */
const char linuxu_dma_ops = 0;
void linuxu_bug(const char *file, int line) { (void)file; (void)line; abort(); }
void linuxu_warn(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }

static unsigned int releases;
static void count_release(void *arg) { (void)arg; releases++; }
static void recursive_release(void *arg)
{
	releases++;
	devres_release_all(arg);
}

static void check_platform_failure(long fail_after, size_t baseline)
{
	struct platform_device *pdev;
	dext_heap_test_fail_after(fail_after);
	pdev = platform_device_register_simple("amdgpu_xcp", 1, NULL, 0);
	dext_heap_test_fail_after(-1);
	if (!IS_ERR(pdev)) {
		assert(strcmp(pdev->name, "amdgpu_xcp.1") == 0);
		platform_device_unregister(pdev);
	} else {
		assert(PTR_ERR(pdev) == -ENOMEM);
	}
	assert(dext_heap_test_live_allocations() == baseline);
}

static void check_managed_allocations(size_t baseline)
{
	struct device dev = {0};
	unsigned char *p = devm_kzalloc(&dev, 19, 0);
	assert(p);
	for (unsigned int i = 0; i < 19; i++) assert(p[i] == 0);
	memset(p, 0xff, 19);
	devm_kfree(&dev, p);
	p = devm_kcalloc(&dev, 7, 13, 0);
	assert(p);
	for (unsigned int i = 0; i < 91; i++) assert(p[i] == 0);
	assert(devm_kcalloc(&dev, SIZE_MAX, 2, 0) == NULL);
	char *name = devm_kasprintf(&dev, 0, "%s-%d-%lx", "queue", 7, 0xa5UL);
	assert(name && strcmp(name, "queue-7-a5") == 0);
	struct devres *res = devres_alloc(37, 0);
	assert(res && devres_add(&dev, res) == 0);
	devres_release_all(&dev);
	devres_release_all(&dev);
	assert(dev.devres == NULL);
	assert(dext_heap_test_live_allocations() == baseline);

	for (long fail = 0; fail < 4; fail++) {
		dext_heap_test_fail_after(fail);
		p = devm_kzalloc(&dev, 19, 0);
		dext_heap_test_fail_after(-1);
		devres_release_all(&dev);
		assert(dext_heap_test_live_allocations() == baseline);
	}
}

static void check_reentrant_cleanup(size_t baseline)
{
	struct device dev = {0};
	releases = 0;
	assert(devm_add_action_or_reset(&dev, count_release, NULL) == 0);
	assert(devm_add_action_or_reset(&dev, recursive_release, &dev) == 0);
	devres_release_all(&dev);
	assert(releases == 2 && dev.devres == NULL);
	assert(devm_add_action_or_reset(&dev, NULL, NULL) == -EINVAL);
	assert(devm_add_action(NULL, count_release, NULL) == -ENOMEM);
	for (long fail = 0; fail < 2; fail++) {
		dext_heap_test_fail_after(fail);
		assert(devm_add_action_or_reset(&dev, count_release, NULL) == -ENOMEM);
		dext_heap_test_fail_after(-1);
		devres_release_all(&dev);
	}
	assert(releases == 4);
	assert(dext_heap_test_live_allocations() == baseline);
}

static void check_explicit_action_release(size_t baseline)
{
	struct device dev = {0};
	releases = 0;
	assert(!devm_add_action(&dev, count_release, NULL));
	assert(!devm_add_action(&dev, recursive_release, &dev));
	devm_release_action(&dev, recursive_release, &dev);
	assert(releases == 2 && !dev.devres);
	devm_release_action(&dev, recursive_release, &dev);
	devres_release_all(&dev);
	assert(releases == 2 && dext_heap_test_live_allocations() == baseline);
}

static void check_realloc_failure(size_t baseline)
{
	unsigned char *p = kmalloc(31, 0);
	assert(p);
	memset(p, 0x6b, 31);
	dext_heap_test_fail_after(0);
	assert(krealloc(p, 73, 0) == NULL);
	dext_heap_test_fail_after(-1);
	assert(ksize(p) == 31);
	for (unsigned int i = 0; i < 31; i++) assert(p[i] == 0x6b);
	p = krealloc(p, 73, 0);
	assert(p);
	for (unsigned int i = 0; i < 31; i++) assert(p[i] == 0x6b);
	assert(krealloc(p, 0, 0) == ZERO_SIZE_PTR);
	assert(dext_heap_test_live_allocations() == baseline);
}

static void check_overflow_and_group_close(size_t baseline)
{
	struct flex { unsigned int count; uint64_t entries[]; };
	assert(size_add(SIZE_MAX, 1) == SIZE_MAX);
	assert(kvcalloc(SIZE_MAX / 4 + 1, 4, 0) == NULL);
	assert(size_mul(SIZE_MAX / 2 + 1, 2) == SIZE_MAX);
	assert(size_add_overflows(SIZE_MAX, 1));
	assert(size_mul_overflows(SIZE_MAX / 2 + 1, 2));
	assert(!size_add_overflows(3, 7) && !size_mul_overflows(3, 7));
	assert(kzalloc_objs(uint64_t, SIZE_MAX / 8 + 2) == NULL);
	assert(kzalloc_flex(struct flex, entries, SIZE_MAX / 8) == NULL);
	struct device dev = {0};
	void *group = devres_open_group(&dev, NULL, 0);
	assert(group);
	releases = 0;
	assert(devm_add_action_or_reset(&dev, count_release, NULL) == 0);
	dext_heap_test_fail_after(0);
	devres_close_group(&dev, group);
	dext_heap_test_fail_after(-1);
	assert(devm_add_action_or_reset(&dev, count_release, NULL) == 0);
	assert(devres_release_group(&dev, group) == 1 && releases == 1);
	devres_release_all(&dev);
	assert(releases == 2);
	assert(dext_heap_test_live_allocations() == baseline);
}

static unsigned int child_releases;
static void release_child(struct kobject *child)
{
	child_releases++;
	kfree(child);
}
static const struct kobj_type child_type = { .release = release_child };

static void check_kobject_ownership(size_t baseline)
{
	struct kobject *parent = kobject_create_and_add("parent", NULL);
	struct kset *set = kset_create_and_add("group", NULL, parent);
	struct kobject *child = kzalloc(sizeof(*child), 0);
	assert(parent && set && child);
	child->kset = set;
	assert(kobject_set_name(child, "child") == 0);
	kobject_init(child, &child_type);
	assert(child->kset == set && !strcmp(child->name, "child"));
	assert(kobject_add(child, NULL, NULL) == 0);
	assert(child->parent == &set->kobj);
	/* Membership and default-parent ownership are independent references. */
	assert(refcount_read(&set->kobj.kref) == 3);
	assert(refcount_read(&parent->kref) == 2);
	kobject_put(parent);
	kset_unregister(set);
	struct kobject *found = kset_find_obj(set, "child");
	assert(found == child && refcount_read(&child->kref) == 2);
	kobject_put(child);
	assert(child_releases == 0 && !strcmp(found->parent->name, "group"));
	kobject_put(found);
	assert(child_releases == 1);
	assert(dext_heap_test_live_allocations() == baseline);

	parent = kobject_create_and_add("parent", NULL);
	child = kzalloc(sizeof(*child), 0);
	assert(parent && child);
	child->parent = parent;
	kobject_init(child, &child_type);
	dext_heap_test_fail_after(0);
	assert(kobject_add(child, parent, "%s", "child") == -ENOMEM);
	dext_heap_test_fail_after(-1);
	assert(refcount_read(&parent->kref) == 1);
	assert(kobject_add(child, parent, "%s", "child") == 0);
	assert(refcount_read(&parent->kref) == 2);
	kobject_del(child);
	kobject_del(child);
	assert(refcount_read(&parent->kref) == 1 && !child->parent);
	kobject_put(parent);
	kobject_put(child);
	assert(child_releases == 2);
	assert(dext_heap_test_live_allocations() == baseline);
}

static unsigned device_releases;
static void release_parent_device(struct device *dev)
{
	device_releases++;
	kfree(dev);
}
static void check_device_parent_ownership(size_t baseline)
{
	struct device *parent = kzalloc(sizeof(*parent), 0);
	struct device *child = kzalloc(sizeof(*child), 0);
	assert(parent && child);
	parent->release = release_parent_device;
	child->release = release_parent_device;
	device_initialize(parent);
	device_initialize(child);
	child->parent = parent;
	assert(!device_add(parent) && !device_add(child));
	assert(refcount_read(&parent->kobj.kref) == 2);
	put_device(parent);
	assert(device_releases == 0 && child->parent == parent);
	device_unregister(child);
	assert(device_releases == 2);
	assert(dext_heap_test_live_allocations() == baseline);

	unsigned char *zero = kmalloc(35, __GFP_ZERO);
	assert(zero);
	for (unsigned i = 0; i < 35; ++i) assert(!zero[i]);
	kfree(zero);
	assert(dext_heap_test_live_allocations() == baseline);
}

int main(void)
{
	/* The in-memory sysfs root intentionally lives for the process lifetime. */
	struct kobject *warmup = kobject_create_and_add("warmup", NULL);
	assert(warmup);
	kobject_put(warmup);
	size_t baseline = dext_heap_test_live_allocations();
	for (long fail = 0; fail < 8; fail++) check_platform_failure(fail, baseline);
	for (int i = 0; i < 64; i++) check_platform_failure(-1, baseline);
	check_managed_allocations(baseline);
	check_reentrant_cleanup(baseline);
	check_explicit_action_release(baseline);
	check_realloc_failure(baseline);
	check_overflow_and_group_close(baseline);
	check_kobject_ownership(baseline);
	check_device_parent_ownership(baseline);
	puts("production heap probe cleanup: platform/kobject, managed allocation, reentrant cleanup, realloc failures passed");
	return 0;
}
