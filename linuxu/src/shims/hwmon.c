/* hwmon class devices for upstream PM, after drivers/hwmon/hwmon.c and
 * drivers/base/core.c get_device_parent(): a device registered with
 * hwmon_device_register_with_groups is named hwmon<id> with the smallest
 * free id, lives in its parent's "hwmon" class directory (the path
 * device/hwmon/hwmon<id>/ that Linux tools scan), carries the "name"
 * attribute and the driver's groups, and keeps the driver data its
 * attribute callbacks read with dev_get_drvdata(). */
#include <pthread.h>
#include <string.h>
#include <linux/hwmon.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

struct hwmon_device {
	struct hwmon_device *next;	/* live devices, by id */
	const char *name;
	int id;
	struct kobject *class_dir;
	struct device dev;
};
#define to_hwmon_device(d) container_of(d, struct hwmon_device, dev)

/* One "hwmon" directory per parent, removed with its last device. */
struct hwmon_class_dir {
	struct hwmon_class_dir *next;
	struct kobject *parent, *dir;
	unsigned int devices;
};

static pthread_mutex_t hwmon_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hwmon_device *hwmon_live;
static struct hwmon_class_dir *hwmon_dirs;

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)attr;
	return sysfs_emit(buf, "%s\n", to_hwmon_device(dev)->name);
}
static struct device_attribute dev_attr_name = __ATTR_RO(name);
static struct attribute *hwmon_dev_attrs[] = { &dev_attr_name.attr, NULL };
static umode_t hwmon_dev_attr_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	(void)n;
	struct device *dev = container_of(kobj, struct device, kobj);
	return to_hwmon_device(dev)->name ? attr->mode : 0;
}
static const struct attribute_group hwmon_dev_attr_group = {
	.attrs = hwmon_dev_attrs,
	.is_visible = hwmon_dev_attr_is_visible,
};

static void hwmon_release(struct device *dev) { kfree(to_hwmon_device(dev)); }

/* ida_alloc: the smallest id no live device holds. Caller holds hwmon_lock. */
static int hwmon_id_alloc_locked(struct hwmon_device *hw)
{
	int id = 0;
	struct hwmon_device **link = &hwmon_live;
	for (; *link && (*link)->id == id; link = &(*link)->next) ++id;
	hw->id = id;
	hw->next = *link;
	*link = hw;
	return id;
}

static struct kobject *class_dir_get(struct kobject *parent)
{
	struct hwmon_class_dir *d;
	pthread_mutex_lock(&hwmon_lock);
	for (d = hwmon_dirs; d; d = d->next)
		if (d->parent == parent) { ++d->devices; break; }
	pthread_mutex_unlock(&hwmon_lock);
	if (d) return d->dir;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d) return NULL;
	d->dir = kobject_create_and_add("hwmon", parent);
	if (!d->dir) { kfree(d); return NULL; }
	d->parent = parent;
	d->devices = 1;
	pthread_mutex_lock(&hwmon_lock);
	d->next = hwmon_dirs;
	hwmon_dirs = d;
	pthread_mutex_unlock(&hwmon_lock);
	return d->dir;
}

static void class_dir_put(struct kobject *dir)
{
	struct hwmon_class_dir *d = NULL, **link;
	if (!dir) return;
	pthread_mutex_lock(&hwmon_lock);
	for (link = &hwmon_dirs; *link; link = &(*link)->next)
		if ((*link)->dir == dir) {
			if (!--(*link)->devices) { d = *link; *link = d->next; }
			break;
		}
	pthread_mutex_unlock(&hwmon_lock);
	if (!d) return;
	kobject_del(dir);
	kobject_put(dir);
	kfree(d);
}

/* Return the id while the device is still referenced. */
static void hwmon_forget(struct hwmon_device *hw)
{
	pthread_mutex_lock(&hwmon_lock);
	for (struct hwmon_device **link = &hwmon_live; *link; link = &(*link)->next)
		if (*link == hw) { *link = hw->next; break; }
	pthread_mutex_unlock(&hwmon_lock);
}

struct device *hwmon_device_register_with_groups(struct device *parent,
	const char *name, void *data, const struct attribute_group **groups)
{
	if (name) {
		if (!*name) return ERR_PTR(-EINVAL);
		for (const char *p=name; *p; ++p)
			if (*p=='-' || *p=='*' || *p==' ' || *p=='\t' || *p=='\n')
				return ERR_PTR(-EINVAL);
	}
	struct hwmon_device *hw=kzalloc(sizeof(*hw),GFP_KERNEL);
	if (!hw) return ERR_PTR(-ENOMEM);
	struct device *dev=&hw->dev;
	device_initialize(dev);
	hw->name=name;
	dev->parent=parent; dev->driver_data=data; dev->release=hwmon_release;
	dev->groups=groups;
	pthread_mutex_lock(&hwmon_lock);
	int id=hwmon_id_alloc_locked(hw);
	pthread_mutex_unlock(&hwmon_lock);
	struct kobject *dir=NULL;
	if (parent) {
		dir=class_dir_get(&parent->kobj);
		if (!dir) { hwmon_forget(hw); put_device(dev); return ERR_PTR(-ENOMEM); }
	}
	hw->class_dir=dir;
	int r=kobject_add(&dev->kobj,dir,"hwmon%d",id);
	if (r) { hwmon_forget(hw); put_device(dev); class_dir_put(dir); return ERR_PTR(r); }
	r=sysfs_create_group(&dev->kobj,&hwmon_dev_attr_group);
	if (!r) r=sysfs_create_groups(&dev->kobj,groups);
	if (r) {
		hwmon_forget(hw);
		device_unregister(dev);
		class_dir_put(dir);
		return ERR_PTR(r);
	}
	return dev;
}
void hwmon_device_unregister(struct device *dev)
{
	if (IS_ERR_OR_NULL(dev) || dev->release!=hwmon_release) return;
	struct hwmon_device *hw=to_hwmon_device(dev);
	struct kobject *dir=hw->class_dir;
	sysfs_remove_groups(&dev->kobj,dev->groups);
	/* Upstream frees the id after device_unregister; the last reference
	 * may free the device there, so return it first. */
	hwmon_forget(hw);
	device_unregister(dev);
	class_dir_put(dir);
}
