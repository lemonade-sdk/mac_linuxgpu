/* linuxu: SHIM (third_party/linux/include/linux/device_cgroup.h) */
#ifndef __LINUX_DEVICE_CGROUP_H
#define __LINUX_DEVICE_CGROUP_H

static inline int devcgroup_allowed(struct device *dev)
{
	(void)dev;
	return 1;
}

static inline void devcgroup_remove(struct device *dev)
{
	(void)dev;
}

#endif /* __LINUX_DEVICE_CGROUP_H */
