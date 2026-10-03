/* linuxu shim: kobject — real minimal kobject/kset + struct device
 * container (NOOP/in-memory table,
 * REAL-minimal per task: init/add/release via kobj_type vtable,
 * parent/child links, kobject_put refcounting).
 * Struct shapes match linuxu/headers/linux/kobject.h + device.h. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <rt/class.h>
#include <rt/sysfs.h>

extern const struct dma_map_ops linuxu_dma_ops;

static pthread_mutex_t kobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kobj_root_lock = PTHREAD_MUTEX_INITIALIZER;
static struct kobject *kobj_root; /* in-memory sysfs root (non-NULL) */
static int kobj_root_init;

struct class_registration {
	struct class_registration *next;
	struct class *class;
};
struct created_device {
	struct created_device *next;
	struct device *device;
};
static struct class_registration *registered_classes;
static struct created_device *created_devices;

static bool class_is_registered(const struct class *class)
{
	struct class_registration *r;

	for (r = registered_classes; r; r = r->next)
		if (r->class == class)
			return true;
	return false;
}

bool linuxu_class_is_registered(const struct class *class)
{
	bool registered;
	pthread_mutex_lock(&kobj_lock);
	registered = class_is_registered(class);
	pthread_mutex_unlock(&kobj_lock);
	return registered;
}

/* name storage: static buffer per live name */
#define KOBJ_NAME_MAX 128
static void kobj_root_ensure(void)
{
	pthread_mutex_lock(&kobj_root_lock);
	if (!kobj_root_init) {
		kobj_root = kzalloc(sizeof(*kobj_root), 0);
		if (kobj_root) {
			kobject_init(kobj_root, NULL);
			kobj_root->name = "linuxu";
			kobj_root_init = 1;
		}
	}
	pthread_mutex_unlock(&kobj_root_lock);
}

/* ---- core ---- */
int kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	if (!kobj)
		return -EINVAL;
	/* Callers may set the name, parent or kset before initialization. */
	INIT_LIST_HEAD(&kobj->entry);
	INIT_LIST_HEAD(&kobj->default_attrs);
	INIT_LIST_HEAD(&kobj->dyn_attrs);
	refcount_set(&kobj->kref, 1);
	kobj->ktype = ktype;
	kobj->state = 0;
	kobj->sd = NULL;
	return 0;
}

struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj)
		refcount_inc(&kobj->kref);
	return kobj;
}

void kobject_put(struct kobject *kobj)
{
	if (!kobj)
		return;
	if (refcount_dec_and_test(&kobj->kref))
		kobject_cleanup(kobj);
}

int kobject_add(struct kobject *kobj, struct kobject *parent,
		 const char *fmt, ...)
{
	char buf[KOBJ_NAME_MAX];
	va_list ap;

	if (!kobj)
		return -EINVAL;
	if (kobj->state & 1)
		return -EEXIST;
	kobj_root_ensure();
	if (!kobj_root)
		return -ENOMEM;
	if (parent)
		kobj->parent = parent;
	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		if (kobject_set_name(kobj, "%s", buf))
			return -ENOMEM;
	}
	if (!kobj->entry.next || !kobj->entry.prev)
		INIT_LIST_HEAD(&kobj->entry);
	int sysfs_error = linuxu_sysfs_add(kobj);
	if (sysfs_error) return sysfs_error;
	const struct attribute_group **groups = kobj->ktype ? kobj->ktype->default_groups : NULL;
	if (groups) for (size_t i = 0; groups[i]; ++i) {
		sysfs_error = sysfs_create_group(kobj, groups[i]);
		if (sysfs_error) {
			linuxu_sysfs_remove(kobj);
			return sysfs_error;
		}
	}
	if (kobj->kset) {
		if (!kobj->parent)
			kobj->parent = &kobj->kset->kobj;
		kobject_get(&kobj->kset->kobj);
		spin_lock(&kobj->kset->list_lock);
		if (list_empty(&kobj->entry))
			list_add_tail(&kobj->entry, &kobj->kset->list);
		spin_unlock(&kobj->kset->list_lock);
	}
	kobject_get(kobj->parent);
	if (kobj->parent) {
		/* link into parent's child tree (skeleton: first-child) */
		kobj->root = kobj->parent->root ? kobj->parent->root :
			kobj->parent;
	} else {
		kobj->root = kobj_root;
	}
	kobj->state = 1; /* KOBJ_ADD */
	return 0;
}
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
			 struct kobject *parent, const char *fmt, ...)
{
	va_list ap;
	int err;

	kobject_init(kobj, ktype);
	kobj->parent = parent;
	va_start(ap, fmt);
	err = kobject_set_name_vargs(kobj, fmt, (void *)&ap);
	va_end(ap);
	if (err)
		return err;
	return kobject_add(kobj, parent, NULL);
}

int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list ap;
	int err;

	if (!kobj || !fmt)
		return -EINVAL;
	va_start(ap, fmt);
	err = kobject_set_name_vargs(kobj, fmt, (void *)&ap);
	va_end(ap);
	return err;
}

int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
			   void *args)
{
	char buf[KOBJ_NAME_MAX];
	char *name;
	va_list ap;

	if (!kobj || !fmt || !args)
		return -EINVAL;
	ap = *(va_list *)args;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	name = malloc(strlen(buf) + 1);
	if (!name)
		return -ENOMEM;
	strcpy(name, buf);
	if (kobj->name_owned)
		free((void *)kobj->name);
	kobj->name = name;
	kobj->name_owned = true;
	return 0;
}

const char *kobject_name(const struct kobject *kobj)
{
	return kobj ? kobj->name : "";
}

int kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
	(void)kobj;
	(void)action;
	return 0; /* no uevents in the dext */
}

static void created_kobject_release(struct kobject *kobj)
{
	kfree(kobj);
}

/* lib/kobject.c dynamic_kobj_ktype. */
static const struct kobj_type created_kobject_ktype = {
	.release = created_kobject_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

struct kobject *kobject_create(void)
{
	struct kobject *kobj = kzalloc(sizeof(*kobj), 0);

	if (!kobj)
		return NULL;
	kobject_init(kobj, &created_kobject_ktype);
	return kobj;
}

struct kobject *kobject_create_and_add(const char *name,
				       struct kobject *parent)
{
	struct kobject *kobj = kobject_create();

	if (!kobj)
		return NULL;
	kobj->parent = parent;
	kobject_set_name(kobj, "%s", name ? name : "kobj");
	if (kobj->name == NULL) {
		kobject_put(kobj);
		return NULL;
	}
	if (kobject_add(kobj, parent, NULL)) {
		kobject_put(kobj);
		return NULL;
	}
	return kobj;
}

struct kobject *kobject_create_and_add_ns(const char *name,
					 const void *ns,
					 struct kobject *parent)
{
	(void)ns;
	return kobject_create_and_add(name, parent);
}

static struct kobject *kobject_detach(struct kobject *kobj)
{
	struct kobject *parent;
	linuxu_sysfs_remove(kobj);
	if (!kobj || !(kobj->state & 1))
		return NULL;
	parent = kobj->parent;
	kobj->parent = NULL;
	kobj->root = NULL;
	kobj->state &= ~1;
	if (kobj->kset && kobj->entry.next && kobj->entry.prev) {
		spin_lock(&kobj->kset->list_lock);
		if (!list_empty(&kobj->entry))
			list_del_init(&kobj->entry);
		spin_unlock(&kobj->kset->list_lock);
		/* Discovery may already have detached entry while walking a kset;
		 * its membership reference still belongs to this object. */
		kobject_put(&kobj->kset->kobj);
	}
	return parent;
}

void kobject_cleanup(struct kobject *kobj)
{
	const char *owned_name = kobj->name_owned ? kobj->name : NULL;
	struct kobject *parent = kobject_detach(kobj);
	if (kobj->ktype && kobj->ktype->release)
		kobj->ktype->release(kobj);
	free((void *)owned_name);
	kobject_put(parent);
}

void kobject_del(struct kobject *kobj)
{
	kobject_put(kobject_detach(kobj));
}

int kobject_rename(struct kobject *kobj, const char *new_name)
{
	if (!kobj || !new_name)
		return -EINVAL;
	return kobject_set_name(kobj, "%s", new_name);
}

void kobject_set_parent(struct kobject *kobj, struct kobject *parent)
{
	kobj->parent = parent;
}

struct kobject *kobject_get_parent(struct kobject *kobj)
{
	return kobj->parent;
}

int kobject_set_sysfs(struct kobject *kobj) { (void)kobj; return 0; }
int kobject_clear_sysfs(struct kobject *kobj) { (void)kobj; return 0; }
int kobject_get_sysfs(struct kobject *kobj) { (void)kobj; return 0; }
int kobject_put_sysfs(struct kobject *kobj) { (void)kobj; return 0; }

int kobject_set_default_attrs(struct kobject *kobj,
			      const struct attribute * const *attrs)
{
	(void)attrs;
	kobj->state |= 2;
	return 0;
}
int kobject_clear_default_attrs(struct kobject *kobj)
{
	kobj->state &= ~2;
	return 0;
}
int kobject_get_default_attrs(struct kobject *kobj)
{
	return kobj->state & 2 ? 0 : -2;
}
int kobject_put_default_attrs(struct kobject *kobj)
{
	kobj->state &= ~2;
	return 0;
}
int kobject_set_dyn_attrs(struct kobject *kobj,
			  const struct attribute * const *attrs)
{
	(void)attrs;
	return 0;
}
int kobject_clear_dyn_attrs(struct kobject *kobj) { return 0; }
int kobject_get_dyn_attrs(struct kobject *kobj)
{
	return -2;
}
int kobject_put_dyn_attrs(struct kobject *kobj) { return 0; }

int kobject_set_priv(struct kobject *kobj, void *priv)
{
	kobj->priv = priv;
	return 0;
}
void *kobject_get_priv(const struct kobject *kobj)
{
	return kobj->priv;
}
int kobject_set_state(struct kobject *kobj, int state)
{
	kobj->state = state;
	return 0;
}
int kobject_get_state(const struct kobject *kobj)
{
	return kobj->state;
}
int kobject_set_flags(struct kobject *kobj, int flags)
{
	kobj->flags = flags;
	return 0;
}
int kobject_get_flags(const struct kobject *kobj)
{
	return kobj->flags;
}
int kobject_set_attr(struct kobject *kobj, const struct attribute *attr)
{
	kobj->attr = attr;
	return 0;
}
const struct attribute *kobject_get_attr(const struct kobject *kobj)
{
	return kobj->attr;
}
int kobject_set_root(struct kobject *kobj, struct kobject *root)
{
	kobj->root = root;
	return 0;
}
struct kobject *kobject_get_root(const struct kobject *kobj)
{
	return kobj->root;
}
int kobject_set_kset(struct kobject *kobj, struct kset *kset)
{
	kobj->kset = kset;
	return 0;
}
struct kset *kobject_get_kset(const struct kobject *kobj)
{
	return kobj->kset;
}
int kobject_set_child(struct kobject *kobj, struct kobject *child)
{
	kobj->child = child;
	return 0;
}
struct kobject *kobject_get_child(const struct kobject *kobj)
{
	return kobj->child;
}

/* ================================================================== *
 * struct device
 * ================================================================== */

static void device_release(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);

	/* get_device references can outlive device_del/unregister. Managed
	 * payloads remain valid until the final reference reaches this point. */
	devres_release_all(dev);
	if (dev && dev->release)
		dev->release(dev);
}

static void created_device_release(struct device *dev)
{
	kfree(dev);
}

/* drivers/base/core.c dev_sysfs_ops: a device's attribute files are
 * struct device_attribute, shown with the device. */
static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct device_attribute *dev_attr =
		container_of(attr, struct device_attribute, attr);
	struct device *dev = container_of(kobj, struct device, kobj);

	return dev_attr->show ? dev_attr->show(dev, dev_attr, buf) : -EIO;
}

static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	struct device_attribute *dev_attr =
		container_of(attr, struct device_attribute, attr);
	struct device *dev = container_of(kobj, struct device, kobj);

	return dev_attr->store ? dev_attr->store(dev, dev_attr, buf, count) : -EIO;
}

static const struct sysfs_ops dev_sysfs_ops = {
	.show = dev_attr_show,
	.store = dev_attr_store,
};

static const struct kobj_type device_ktype = {
	.release = device_release,
	.sysfs_ops = &dev_sysfs_ops,
};

void device_initialize(struct device *dev)
{
	struct kobj_type *kt = dev->kobj.ktype ? dev->kobj.ktype :
						&device_ktype;

	kobject_init(&dev->kobj, kt);
	INIT_LIST_HEAD(&dev->links);
	INIT_LIST_HEAD(&dev->children);
	INIT_LIST_HEAD(&dev->alldevices);
	refcount_set(&dev->removable, 1);
	dev->pinned = 0;
	dev->power.suspended = 0;
	INIT_LIST_HEAD(&dev->power.power_entry);
	spin_lock_init(&dev->lock);
	/* Platform DMA ownership operations (linuxu/src/drm/ttm.c). */
	dev->dma_ops = &linuxu_dma_ops;
}
/* struct device in the header has no plain refcount member besides
 * kobj.kref; use kobj.kref for get/put. */

struct device *get_device(struct device *dev)
{
	if (dev)
		kobject_get(&dev->kobj);
	return dev;
}

void put_device(struct device *dev)
{
	if (dev)
		kobject_put(&dev->kobj);
}

int device_add(struct device *dev)
{
	if (!dev) return -EINVAL;
	int result = kobject_add(&dev->kobj, dev->parent ? &dev->parent->kobj : NULL, NULL);
	if (result) return result;
	if (dev->groups) for (size_t i = 0; dev->groups[i]; ++i) {
		result = sysfs_create_group(&dev->kobj, dev->groups[i]);
		if (result) { kobject_del(&dev->kobj); return result; }
	}
	return 0;
}

void device_del(struct device *dev)
{
	kobject_del(&dev->kobj);
}

void device_destroy(struct class *class, dev_t devt)
{
	struct created_device **link;
	struct created_device *r = NULL;

	pthread_mutex_lock(&kobj_lock);
	for (link = &created_devices; *link; link = &(*link)->next) {
		if ((*link)->device->class == class &&
		    (*link)->device->devt == devt) {
			r = *link;
			*link = r->next;
			break;
		}
	}
	pthread_mutex_unlock(&kobj_lock);
	if (r) {
		device_del(r->device);
		put_device(r->device);
		free(r);
	}
}

struct device *device_create(struct class *class, struct device *parent,
			     dev_t devt, void *driver_data,
			     const char *fmt, ...)
{
	char buf[64];
	struct device *dev;
	struct created_device *record;
	va_list ap;
	int err;

	if (!class || !fmt)
		return ERR_PTR(-EINVAL);
	dev = kzalloc(sizeof(*dev), 0);
	if (!dev)
		return ERR_PTR(-ENOMEM);
	device_initialize(dev);
	dev->release = created_device_release;
	dev->devt = devt;
	dev->parent = parent;
	dev->class = class;
	dev->driver_data = driver_data;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	err = kobject_set_name(&dev->kobj, "%s", buf);
	if (err)
		goto fail_device;
	record = malloc(sizeof(*record));
	if (!record) {
		err = -ENOMEM;
		goto fail_device;
	}
	pthread_mutex_lock(&kobj_lock);
	if (!class_is_registered(class)) {
		err = -ENODEV;
		goto fail_locked;
	}
	for (struct created_device *r = created_devices; r; r = r->next)
		if (r->device->class == class && r->device->devt == devt) {
			err = -EEXIST;
			goto fail_locked;
		}
	err = kobject_add(&dev->kobj, parent ? &parent->kobj : NULL, NULL);
	if (err)
		goto fail_locked;
	record->device = dev;
	record->next = created_devices;
	created_devices = record;
	pthread_mutex_unlock(&kobj_lock);
	return dev;

fail_locked:
	pthread_mutex_unlock(&kobj_lock);
	free(record);
fail_device:
	put_device(dev);
	return ERR_PTR(err);
}

int device_register(struct device *dev)
{
	device_initialize(dev);
	return device_add(dev);
}

void device_unregister(struct device *dev)
{
	device_del(dev);
	put_device(dev);
}

struct device *device_find_child(struct device *parent, void *data,
				 bool (*match)(struct device *dev,
					       void *data))
{
	struct list_head *pos;

	if (!parent)
		return NULL;
	list_for_each(pos, &parent->children) {
		struct device *child =
			container_of(pos, struct device, children);
		/* children list holds the *node*; we store dev in the
		 * list itself for the skeleton */
		if (match(child, data))
			return child;
	}
	return NULL;
}

void device_for_each_child(struct device *parent, void *data,
			   void (*fn)(struct device *dev, void *data))
{
	struct list_head *pos;

	if (!parent)
		return;
	list_for_each(pos, &parent->children) {
		struct device *child =
			container_of(pos, struct device, children);
		fn(child, data);
	}
}

/* ---- class / bus (in-memory table) ---- */
struct class *class_create(const char *name)
{
	struct class *c;
	int err;

	if (!name)
		return ERR_PTR(-EINVAL);
	c = kzalloc(sizeof(*c), 0);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->name = name;
	err = class_register(c);
	if (err) {
		kfree(c);
		return ERR_PTR(err);
	}
	return c;
}

void class_destroy(struct class *cls)
{
	if (IS_ERR_OR_NULL(cls))
		return;
	class_unregister(cls);
	kfree(cls);
}

int class_register(struct class *cls)
{
	struct class_registration *record;
	struct class_registration *r;

	if (!cls || !cls->name)
		return -EINVAL;
	record = malloc(sizeof(*record));
	if (!record)
		return -ENOMEM;
	pthread_mutex_lock(&kobj_lock);
	for (r = registered_classes; r; r = r->next)
		if (r->class == cls || strcmp(r->class->name, cls->name) == 0) {
			pthread_mutex_unlock(&kobj_lock);
			free(record);
			return -EEXIST;
		}
	record->class = cls;
	record->next = registered_classes;
	registered_classes = record;
	pthread_mutex_unlock(&kobj_lock);
	return 0;
}

void class_unregister(struct class *cls)
{
	struct class_registration **link;
	struct class_registration *record = NULL;
	struct created_device *detached = NULL;
	struct created_device **device_link;

	if (!cls)
		return;
	pthread_mutex_lock(&kobj_lock);
	for (link = &registered_classes; *link; link = &(*link)->next)
		if ((*link)->class == cls) {
			record = *link;
			*link = record->next;
			break;
		}
	for (device_link = &created_devices; *device_link;) {
		struct created_device *r = *device_link;
		if (r->device->class == cls) {
			*device_link = r->next;
			r->next = detached;
			detached = r;
		} else {
			device_link = &r->next;
		}
	}
	pthread_mutex_unlock(&kobj_lock);
	while (detached) {
		struct created_device *r = detached;
		detached = r->next;
		device_del(r->device);
		put_device(r->device);
		free(r);
	}
	linuxu_sysfs_remove_class(cls);
	free(record);
}

int bus_register(struct bus_type *bus)
{
	(void)bus;
	return 0;
}

void bus_unregister(struct bus_type *bus)
{
	(void)bus;
}

void bus_for_each_dev(struct bus_type *bus, struct device *start,
		      void *data, int (*fn)(struct device *dev, void *data))
{
	(void)bus; (void)start; (void)data; (void)fn;
}

/* ---- kset (vendor 2026 API; amdgpu_discovery.c ip discovery ksets) ----
 * kobject_add/kobject_set_name carry the (kobj, parent, fmt, ...) and
 * (kobj, fmt, ...) signatures, so these helpers must pass NULL for the
 * trailing format argument. */
void kset_init(struct kset *kset)
{
	kobject_init(&kset->kobj, kset->kobj.ktype);
	INIT_LIST_HEAD(&kset->list);
	spin_lock_init(&kset->list_lock);
}

int kset_register(struct kset *kset)
{
	if (!kset)
		return -EINVAL;
	if (kset->kobj.state & 1)
		return -EEXIST;
	/* Discovery fills the embedded kobject's name, parent kset, and release
	 * type before registration.  Initialize only the list/ref bookkeeping. */
	kset_init(kset);
	return kobject_add(&kset->kobj, kset->kobj.parent, NULL);
}

void kset_unregister(struct kset *kset)
{
	if (kset)
		kobject_put(&kset->kobj);
}

static void created_kset_release(struct kobject *kobj)
{
	kfree(to_kset(kobj));
}

/* lib/kobject.c kset_ktype. */
static const struct kobj_type created_kset_ktype = {
	.release = created_kset_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

struct kset *kset_create_and_add(const char *name,
				 const struct kset_uevent_ops *u,
				 struct kobject *parent_kobj)
{
	struct kset *kset = kzalloc(sizeof(*kset), 0);

	if (!kset)
		return NULL;
	kobject_init(&kset->kobj, &created_kset_ktype);
	kset->kobj.parent = parent_kobj;
	if (kobject_set_name(&kset->kobj, "%s", name ? name : "kset") ||
	    kset_register(kset)) {
		kobject_put(&kset->kobj);
		return NULL;
	}
	(void)u;
	return kset;
}

struct kobject *kset_find_obj(struct kset *k, const char *name)
{
	struct list_head *p;
	struct kobject *result = NULL;

	if (!k)
		return NULL;
	spin_lock(&k->list_lock);
	list_for_each(p, &k->list) {
		struct kobject *kobj = container_of(p, struct kobject, entry);

		if (kobj->name && name && !strcmp(kobj->name, name) &&
		    refcount_inc_not_zero(&kobj->kref)) {
			result = kobj;
			break;
		}
	}
	spin_unlock(&k->list_lock);
	return result;
}
