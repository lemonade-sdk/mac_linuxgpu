/* linuxu: SHIM (third_party/linux/include/linux/component.h) */
#ifndef __LINUX_COMPONENT_H
#define __LINUX_COMPONENT_H

struct device;

struct component_ops {
	int (*bind)(struct device *dev, struct device *master, void *data);
	void (*unbind)(struct device *dev, struct device *master, void *data);
	void (*event)(struct device *dev, void *data);
};

extern int component_add(struct device *dev, const struct component_ops *ops);
extern void component_del(struct device *dev, const struct component_ops *ops);

#endif
