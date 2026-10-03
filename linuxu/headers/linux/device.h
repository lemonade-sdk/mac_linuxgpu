/* linuxu: SHIM (third_party/linux/include/linux/device.h)
 *
 * THE keystone: ttm/drm/pci/i2c/debugfs all hang off struct device.
 * Keeps the upstream field order for the fields the driver touches
 * (driver/driver_data/parent/kobj/class/lock).
 */
#ifndef __LINUX_DEVICE_H
#define __LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/cleanup.h>
#include <linux/pm.h>

typedef int suspend_state_t;
#include <linux/kobject.h>
#include <linux/pm_domain.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/mod_devicetable.h>
#include <linux/dev_printk.h>   /* linuxu: dev_err_probe (drm_bridge.c) */
#include <linux/vga_switcheroo.h>   /* linuxu: video_is_primary_device (drm_sysfs.c) */
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>

struct device;
struct device_driver;
struct module;
struct devres;
struct class_attribute;
struct bus_type;
struct class;
struct driver;
struct firmware;
struct dev_pin {
	struct list_head	node;
	int			flags;
};
struct dev_pin_list {
	struct list_head	list;
	int			count;
};

struct dev_pm_info {
	unsigned int		suspended;
	int			suspend_state;
	struct list_head	power_entry;
	int			disable_depth;
};

struct iommu_group;

struct fwnode_handle {
	struct fwnode_handle *secondary;
};

struct device {

	dev_t			devt;
	struct kobject		kobj;			/* sysfs */
	const char		*init_name;
	struct device		*parent;
	struct bus_type		*bus;
	struct device_driver	*driver;
	void			*platform_data;
	void			*driver_data;
	struct list_head	links;
	struct list_head	children;
	refcount_t		removable;
	unsigned		pinned:1;
	unsigned int		id;
	spinlock_t		lock;
	struct kobject		*kobj_parent;
	struct class		*class;
	struct device_node	*of_node;
	struct fwnode_handle	*fwnode;
	u64			*dma_mask;
	u64			coherent_dma_mask;
	u64			bus_dma_limit;
	size_t			max_dma_size;
	const char		*bus_id;
	struct iommu_group	*iommu_group;
	struct dev_pm_info	power;
	const struct attribute_group	**groups;
	void			(*release)(struct device *dev);
	struct list_head	alldevices;
	struct devres		*devres;
	const struct dma_map_ops *dma_ops;	/* vendor 2026 device.h */
	const struct device_type *type;		/* vendor 2026 device.h */
};

struct dev_pm_ops;

/*
 * struct device_driver — the 2026 layout adds `.pm` (vendor
 * device/driver.h); amdgpu_drv.c uses `.driver.pm = pm_ptr(&amdgpu_pm_ops)`
 * with an embedded struct pci_driver.
 */
struct device_driver {
	const char		*name;
	struct bus_type		*bus;
	struct module		*owner;
	const char		*mod_name;
	bool			suppress_bind_attrs;
	const struct device_id	*id_table;
	int			(*probe)(struct device *dev);
	void			(*sync_state)(struct device *dev);
	int			(*remove)(struct device *dev);
	void			(*shutdown)(struct device *dev);
	const struct dev_pm_ops	*pm;
	void			*driver_data;
	const struct attribute_group **dev_groups;
	const struct pci_error_handlers *err_handler;
};

struct class {
	struct module		*owner;
	const char		*name;
	const struct class_attribute	*class_attrs;
	const struct attribute_group	**class_groups;
	void			(*dev_release)(struct device *dev);
	char *(*devnode)(const struct device *dev, umode_t *mode);
};

#define dev_name(dev)		((dev)->kobj.name)
#define dev_set_name(dev, fmt, ...) 	kobject_set_name(&(dev)->kobj, fmt, ##__VA_ARGS__)
#define dev_parent(dev)		((dev)->parent)
#define dev_set_parent(dev, parent) 	do { (dev)->parent = (parent); } while (0)
#define dev_fwnode(dev)		((dev)->fwnode)
#define dev_set_of_node(dev, node) 	do { (dev)->of_node = (node); } while (0)
#define dev_get_of_node(dev)	((dev)->of_node)
#define dev_printk(level, dev, fmt, ...) \
	dev_printk_impl(level, (struct device *)(dev) ? dev_name((struct device *)(dev)) : "device", fmt, ##__VA_ARGS__)
void dev_printk_impl(const char *level, const char *dev_name,
		    const char *fmt, ...) __printf(3, 4);
#define dev_emerg(dev, fmt, ...) 		dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...) 		dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...) 		dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) 		dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...) 	dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) 		dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...) 	dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) 		dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) 		dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...) 	dev_info(dev, fmt, ##__VA_ARGS__)

#define dev_get_drvdata(dev)	((dev)->driver_data)
#define dev_set_drvdata(dev, data) 	do { (dev)->driver_data = (data); } while (0)
#define dev_get_regmap(dev)	NULL
#define dev_get_platform_data(dev) ((dev)->platform_data)

extern void device_initialize(struct device *dev);
extern int device_add(struct device *dev);
extern void device_del(struct device *dev);
extern void device_destroy(struct class *class, dev_t devt);
extern struct device *device_create(struct class *class, struct device *parent,
				    dev_t devt, void *driver_data,
				    const char *fmt, ...);
extern int device_register(struct device *dev);
extern void device_unregister(struct device *dev);
extern struct device *get_device(struct device *dev);
extern void put_device(struct device *dev);
extern int device_init(struct device *dev);
extern struct device *device_find_child(struct device *parent,
					void *data,
					bool (*match)(struct device *dev, void *data));
extern void device_for_each_child(struct device *parent, void *data,
				  void (*fn)(struct device *dev, void *data));
extern struct class *class_create(const char *name);
extern void class_destroy(struct class *cls);
extern int class_register(struct class *cls);
extern void class_unregister(struct class *cls);
extern int bus_register(struct bus_type *bus);
extern void bus_unregister(struct bus_type *bus);
extern void bus_for_each_dev(struct bus_type *bus, struct device *start,
			     void *data, int (*fn)(struct device *dev, void *data));
extern void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
extern void devm_kfree(struct device *dev, void *p);
extern char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...) __printf(3, 4);
extern void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
extern void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp);
extern int devm_add_action_or_reset(struct device *dev,
				    void (*cleanup)(void *data), void *data);
extern int devm_add_action(struct device *dev, void (*cleanup)(void *data), void *data);
extern void devm_release_resource(struct device *dev);
struct resource;
extern void *devm_ioremap_resource(struct device *dev, struct resource *res);
extern void *devm_ioremap(struct device *dev, phys_addr_t addr, size_t size);
extern int devm_device_add_group(struct device *dev, const struct attribute_group *group);
extern void devm_device_remove_group(struct device *dev, const struct attribute_group *group);
extern void devres_init(struct device *dev);
extern struct devres *devres_alloc(size_t size, gfp_t gfp);
extern void devres_free(struct devres *res);
extern void devres_destroy(struct devres *res);
extern void devres_release(struct devres *res);
extern int devres_add(struct device *dev, struct devres *res);
extern void devres_release_all(struct device *dev);
extern void *devres_open_group(struct device *dev, void *id, gfp_t gfp);
extern void devres_close_group(struct device *dev, void *id);
extern int devres_release_group(struct device *dev, void *id);
extern int devm_request_irq(struct device *dev, unsigned int irq,
			    irq_handler_t handler, unsigned long flags,
			    const char *name, void *dev_id);
extern int devm_request_threaded_irq(struct device *dev, unsigned int irq,
				     irq_handler_t handler, irq_handler_t thread_fn,
				     unsigned long flags, const char *name, void *dev_id);
extern void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
extern struct i2c_adapter *devm_i2c_new_device(struct device *parent,
						struct i2c_board_info const *info);
extern void devm_i2c_delete_adapter(struct device *dev, struct i2c_adapter *adap);
extern int devm_i2c_add_numbered_adapter(struct device *dev,
					 struct i2c_adapter *adap, int nr);
extern void devm_i2c_del_adapter(struct device *dev, struct i2c_adapter *adap);
extern int devm_i2c_register_adapter(struct device *dev, struct i2c_adapter *adap);

static inline const char *dev_driver_string(struct device *dev) { (void)dev; return "unknown"; }
static inline bool dev_is_removable(struct device *dev) { (void)dev; return false; }


/* ---- sysfs file surface (runtime: linuxu/src/shims/sysfs.c) ---- */
extern int device_create_file(struct device *dev, const struct device_attribute *attr);
extern void device_remove_file(struct device *dev, const struct device_attribute *attr);
extern int device_create_bin_file(struct device *dev, const struct bin_attribute *attr);
extern void device_remove_bin_file(struct device *dev, const struct bin_attribute *attr);
extern int device_add_groups(struct device *dev, const struct attribute_group **groups);
extern void device_remove_groups(struct device *dev, const struct attribute_group **groups);

/* content previously appended after the guard; dev_err_probe is already
 * provided by linux/dev_printk.h */

#ifndef device_is_registered
static inline bool device_is_registered(struct device *dev)
{
	return dev && (dev->kobj.state & 1) && dev->kobj.sd;
}
#endif

extern struct bus_type pci_bus_type;
static inline bool dev_is_pci(struct device *dev)
{
	return dev && dev->bus == &pci_bus_type;
}

struct bus_type {
	const char *name;
};


void devm_release_action(struct device *dev, void (*release)(void *data), void *data);

struct device_type {
	const char *name;
	void (*init)(struct device *dev);
	void (*release)(struct device *dev);
	int (*uevent)(struct device *dev, void *data);
	const struct attribute_group **groups;
	const struct bus_type *bus;
	const struct class *class;
	const struct device_group *dev_groups;
	const struct of_device_id *of_match_table;
	void (*suspend)(struct device *dev, void *state);
	void (*resume)(struct device *dev);
};

#ifndef _LINUXU_DEV_TO_NODE
#define _LINUXU_DEV_TO_NODE
static inline int dev_to_node(const struct device *dev)
{
	(void)dev;
	return 0;
}
#endif /* _LINUXU_DEV_TO_NODE */


struct platform_device;
void platform_device_unregister(struct platform_device *pdev);

void fwnode_handle_put(struct fwnode_handle *fwnode);
#endif /* __LINUX_DEVICE_H */

/* DEVICE_ATTR_RO - read-only device attribute */
#ifndef DEVICE_ATTR_RO
#define DEVICE_ATTR_RO(_name) \
	__DEVICE_ATTR(_name, 0444, NULL, NULL)
#define __DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = { \
		.attr = { .name = #_name, .mode = _mode }, \
		.show = _show, .store = _store \
	}
#endif
extern u64 ktime_get_mono_fast_ns(void);
extern bool cancel_work(struct work_struct *work);
extern void ksys_sync_helper(void);
extern void emergency_restart(void);
