/* linuxu: SHIM (third_party/linux/include/linux/devcoredump.h) */
#ifndef __LINUX_DEVCOREDUMP_H
#define __LINUX_DEVCOREDUMP_H

#include <linux/types.h>
#include <linux/device.h>

struct devcd_info {
	const void *data;
	size_t size;
	struct device *dev;
	int (*release)(struct device *dev, struct devcd_info *info);
};

static inline void devcd_set_release(struct devcd_info *info,
				     int (*release)(struct device *,
						     struct devcd_info *))
{
	info->release = release;
}

static inline int devcoredump(struct device *dev, const void *data,
			      size_t size, gfp_t gfp)
{
	(void)dev; (void)data; (void)size; (void)gfp;
	return 0;
}

static inline void devcd_free(struct devcd_info *info)
{
	(void)info;
}

/* No exporter retains the payload: its head (the device, the ring and
 * process that hung) goes to the kernel log at error level, which reaches
 * the unified log as events (linuxu/src/shims/dev_coredump.c); then the
 * transferred ownership is honored. */
void dev_coredumpm(struct device *dev, struct module *owner,
		   void *data, size_t datalen, gfp_t gfp,
		   ssize_t (*read)(char *buffer, loff_t offset, size_t count,
				   void *data, size_t datalen),
		   void (*free)(void *data));

#endif /* __LINUX_DEVCOREDUMP_H */
