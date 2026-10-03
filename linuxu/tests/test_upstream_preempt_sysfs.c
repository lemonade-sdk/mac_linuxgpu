/* Unchanged upstream PREEMPT manager + production in-process sysfs lifecycle.
 * No endpoint, DriverKit or GPU operation is linked into this fixture. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "amdgpu.h"
#include <rt/sysfs.h>

static int legacy_registration, evict_error, releases, allocations_until_failure = -1;
void *sysfs_test_calloc(size_t count, size_t size)
{
    if (allocations_until_failure == 0) return NULL;
    if (allocations_until_failure > 0) --allocations_until_failure;
    return calloc(count, size);
}
const struct dma_map_ops linuxu_dma_ops = {0};
static int fixture_device_create_file(struct device *dev, const struct device_attribute *attr);
#define device_create_file fixture_device_create_file
#include "../../third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_preempt_mgr.c"
#undef device_create_file
#include "preempt_resource_production.inc"
static int fixture_device_create_file(struct device *dev, const struct device_attribute *attr)
{
    return legacy_registration ? -ENOSYS : device_create_file(dev, attr);
}
/* PREEMPT fini may evict real BOs. This empty-manager fixture admits no BO or
 * fence, and exposes an explicit failure to check upstream's refusal branch. */
int ttm_resource_manager_evict_all(struct ttm_device *bdev, struct ttm_resource_manager *man)
{
    assert(man->bdev == bdev && !man->usage);
    for (int i = 0; i < TTM_MAX_BO_PRIORITY; ++i) assert(list_empty(&man->lru[i]));
    return evict_error;
}
void ttm_resource_init(struct ttm_buffer_object *bo, const struct ttm_place *place, struct ttm_resource *res)
{ (void)bo; (void)place; (void)res; assert(!"fixture must not allocate GPU resources"); }
void ttm_resource_fini(struct ttm_resource_manager *man, struct ttm_resource *res)
{ (void)man; (void)res; assert(!"fixture must not release GPU resources"); }
void dma_fence_release(struct kref *ref) { (void)ref; assert(!"fixture owns no fences"); }
static void device_released(struct device *dev) { (void)dev; ++releases; }
static void init_device(struct device *dev)
{
    memset(dev, 0, sizeof(*dev));
    device_initialize(dev); dev->release = device_released;
    assert(!dev_set_name(dev, "offline-pci"));
    assert(!device_add(dev));
    assert(device_is_registered(dev) && dev->kobj.sd);
}
static ssize_t writable_show(struct device *dev, struct device_attribute *attr, char *buf)
{ (void)dev; (void)attr; return sysfs_emit(buf, "real callback\n"); }
static ssize_t writable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{ (void)attr; (void)buf; dev->id += size; return size; }
static DEVICE_ATTR_RW(writable);
static struct attribute first = { "first", 0444 }, second = { "second", 0444 };
static struct attribute *group_attrs[] = { &first, &second, NULL };
static int show_second;
static umode_t visibility(struct kobject *kobj, struct attribute *attr, int index)
{ (void)kobj; (void)attr; return index && !show_second ? 0 : 0444; }
static const struct attribute_group group = { .name = "memory", .attrs = group_attrs, .is_visible = visibility };
static unsigned action_releases;
static void release_action(void *data) { assert(data == &action_releases); ++action_releases; }
static void *register_until_detached(void *arg)
{
    struct device *dev = arg;
    for (unsigned i = 0; i < 1000; ++i) {
        int result = device_create_file(dev, &dev_attr_writable);
        assert(result == 0 || result == -EEXIST || result == -ENOENT);
        device_remove_file(dev, &dev_attr_writable);
    }
    return NULL;
}
static void lifetime(void)
{
    struct device dev; init_device(&dev);
    assert(!devm_device_add_group(&dev, &group));
    assert(!devm_add_action(&dev, release_action, &action_releases));
    get_device(&dev);
    const int old_releases = releases;
    device_unregister(&dev);
    assert(!dev.kobj.sd && !linuxu_sysfs_count(&dev.kobj));
    assert(dev.devres && !action_releases && releases == old_releases);
    put_device(&dev);
    assert(!dev.devres && action_releases == 1 && releases == old_releases + 1);

    init_device(&dev);
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, register_until_detached, &dev));
    device_del(&dev); /* stack device remains owned until every thread joins */
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    assert(!linuxu_sysfs_count(&dev.kobj));
    put_device(&dev);

    memset(&dev, 0, sizeof(dev)); device_initialize(&dev); dev.release = device_released;
    const struct attribute_group *groups[] = { &group, NULL };
    dev.groups = groups; show_second = 1;
    allocations_until_failure = 2;
    assert(device_add(&dev) == -ENOMEM && !dev.kobj.sd && !linuxu_sysfs_count(&dev.kobj));
    allocations_until_failure = -1;
    assert(!device_add(&dev));
    assert(linuxu_sysfs_has_file(&dev.kobj, "memory", "second"));
    device_unregister(&dev);

    struct kobject kobj = {0};
    struct kobj_type type = { .default_groups = groups };
    assert(!kobject_init_and_add(&kobj, &type, NULL, "discovery"));
    assert(linuxu_sysfs_has_file(&kobj, "memory", "first"));
    kobject_put(&kobj);
    assert(!linuxu_sysfs_count(NULL));
}
static void registry(void)
{
    struct device dev = {0}, peer = {0};
    assert(device_create_file(NULL, &dev_attr_writable) == -EINVAL);
    assert(device_create_file(&dev, &dev_attr_writable) == -ENOENT);
    init_device(&dev); init_device(&peer);
    assert(!device_create_file(&dev, &dev_attr_writable));
    struct device_attribute duplicate = { .attr = { "writable", 0444 } };
    assert(device_create_file(&dev, &duplicate) == -EEXIST);
    assert(!device_create_file(&peer, &duplicate));
    assert(linuxu_sysfs_has_file(&dev.kobj, NULL, "writable"));
    struct attribute bad = { "bad/name", 0444 };
    assert(sysfs_create_file(&dev.kobj, &bad) == -EINVAL);
    const struct attribute *transaction[] = { &first, &dev_attr_writable.attr, NULL };
    assert(sysfs_create_files(&dev.kobj, transaction) == -EEXIST);
    assert(!linuxu_sysfs_has_file(&dev.kobj, NULL, "first"));
    for (int fail = 0; fail < 3; ++fail) {
        show_second = 1; allocations_until_failure = fail;
        assert(sysfs_create_group(&dev.kobj, &group) == -ENOMEM);
        assert(linuxu_sysfs_count(&dev.kobj) == 1);
    }
    allocations_until_failure = -1; show_second = 0;
    assert(!sysfs_create_group(&dev.kobj, &group));
    assert(linuxu_sysfs_has_file(&dev.kobj, "memory", "first"));
    assert(!linuxu_sysfs_has_file(&dev.kobj, "memory", "second"));
    assert(sysfs_create_group(&dev.kobj, &group) == -EEXIST);
    show_second = 1; allocations_until_failure = 1;
    assert(sysfs_update_group(&dev.kobj, &group) == -ENOMEM);
    assert(linuxu_sysfs_has_file(&dev.kobj, "memory", "first"));
    assert(!linuxu_sysfs_has_file(&dev.kobj, "memory", "second"));
    allocations_until_failure = -1;
    assert(!sysfs_update_group(&dev.kobj, &group));
    assert(linuxu_sysfs_has_file(&dev.kobj, "memory", "second"));
    sysfs_remove_group(&dev.kobj, &group);
    assert(linuxu_sysfs_count(&dev.kobj) == 1);
    assert(!devm_device_add_group(&dev, &group));
    devm_device_remove_group(&dev, &group);
    assert(linuxu_sysfs_count(&dev.kobj) == 1);
    assert(!devm_device_add_group(&dev, &group));
    devres_release_all(&dev);
    assert(linuxu_sysfs_count(&dev.kobj) == 1);
    assert(!sysfs_create_link(&dev.kobj, &peer.kobj, "peer"));
    assert(!sysfs_rename_link(&dev.kobj, &peer.kobj, "peer", "renamed"));
    device_unregister(&peer);
    assert(!linuxu_sysfs_has_file(&dev.kobj, NULL, "renamed"));
    /* Kobject detach must use its own copied names, not expired attributes. */
    struct bin_attribute *binary = calloc(1, sizeof(*binary));
    binary->attr.name = strdup("binary"); binary->attr.mode = 0444;
    assert(!device_create_bin_file(&dev, binary));
    free((void *)binary->attr.name); free(binary);
    char *buf = malloc(PAGE_SIZE);
    assert(dev_attr_writable.show == writable_show && dev_attr_writable.store == writable_store);
    assert(dev_attr_writable.show(&dev, &dev_attr_writable, buf) == 14);
    assert(!strcmp(buf, "real callback\n"));
    assert(dev_attr_writable.store(&dev, &dev_attr_writable, "abc", 3) == 3 && dev.id == 3);
    assert(sysfs_emit_at(buf, PAGE_SIZE - 3, "%s", "abcd") == 2);
    assert(!strcmp(buf + PAGE_SIZE - 3, "ab"));
    assert(sysfs_emit_at(buf, -1, "x") == 0 && sysfs_emit_at(buf, PAGE_SIZE, "x") == 0);
    free(buf);
    device_unregister(&dev);
    assert(!linuxu_sysfs_count(NULL));
    struct device parent = {0}, child = {0};
    device_initialize(&parent); parent.release = device_released;
    device_initialize(&child); child.release = device_released; child.parent = &parent;
    assert(device_add(&child) == -ENOENT && !child.kobj.sd);
    put_device(&child); put_device(&parent);
}
static void preempt(void)
{
    struct device dev; init_device(&dev);
    struct amdgpu_device *adev = calloc(1, sizeof(*adev)); assert(adev);
    adev->dev = &dev; dev_set_drvdata(&dev, adev_to_drm(adev));
    spin_lock_init(&adev->mman.bdev.lru_lock);
    legacy_registration = 1;
    assert(amdgpu_preempt_mgr_init(adev) == -ENOSYS);
    assert(!ttm_manager_type(&adev->mman.bdev, AMDGPU_PL_PREEMPT));
    assert(!linuxu_sysfs_count(&dev.kobj));
    legacy_registration = 0;
    assert(!amdgpu_preempt_mgr_init(adev));
    assert(ttm_manager_type(&adev->mman.bdev, AMDGPU_PL_PREEMPT) == &adev->mman.preempt_mgr);
    assert(adev->mman.preempt_mgr.use_type && adev->mman.preempt_mgr.use_tt);
    assert(linuxu_sysfs_has_file(&dev.kobj, NULL, "mem_info_preempt_used"));
    char *buf = malloc(PAGE_SIZE); adev->mman.preempt_mgr.usage = 123456;
    assert(dev_attr_mem_info_preempt_used.show == mem_info_preempt_used_show);
    assert(dev_attr_mem_info_preempt_used.show(&dev, &dev_attr_mem_info_preempt_used, buf) == 7);
    assert(!strcmp(buf, "123456\n"));
    adev->mman.preempt_mgr.usage = 0;
    evict_error = -EBUSY; amdgpu_preempt_mgr_fini(adev);
    assert(linuxu_sysfs_has_file(&dev.kobj, NULL, "mem_info_preempt_used"));
    assert(ttm_manager_type(&adev->mman.bdev, AMDGPU_PL_PREEMPT));
    evict_error = 0; amdgpu_preempt_mgr_fini(adev);
    assert(!ttm_manager_type(&adev->mman.bdev, AMDGPU_PL_PREEMPT));
    assert(!linuxu_sysfs_count(&dev.kobj));
    free(buf); free(adev); device_unregister(&dev);
}
int main(void)
{
    registry(); lifetime(); preempt();
    assert(!linuxu_sysfs_count(NULL));
    puts("PASS real upstream PREEMPT: ENOSYS negative control, registration/show/fini; sysfs rollback, copied-name teardown, groups and devres");
}
