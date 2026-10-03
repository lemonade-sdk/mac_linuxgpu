/* linuxu: SHIM (vendor/linux/include/linux/hwmon-sysfs.h)
 *
 * hwmon sysfs attribute wrappers for amdgpu pm (amdgpu_pm.c).
 * SENSOR_DEVICE_ATTR* forms only — the driver's hwmon code uses those.
 */
#ifndef _LINUX_HWMON_SYSFS_H
#define _LINUX_HWMON_SYSFS_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>

struct sensor_device_attribute {
	struct device_attribute dev_attr;
	int index;
};

#define to_sensor_dev_attr(_dev_attr) \
	container_of(_dev_attr, struct sensor_device_attribute, dev_attr)

#define SENSOR_ATTR(_name, _mode, _show, _store, _index)	\
	{ .dev_attr = __ATTR(_name, _mode, _show, _store),	\
	  .index = _index }

#define SENSOR_ATTR_RO(_name, _func, _index)			\
	SENSOR_ATTR(_name, 0444, _func##_show, NULL, _index)

#define SENSOR_ATTR_RW(_name, _func, _index)			\
	SENSOR_ATTR(_name, 0644, _func##_show, _func##_store, _index)

#define SENSOR_ATTR_WO(_name, _func, _index)			\
	SENSOR_ATTR(_name, 0200, NULL, _func##_store, _index)

#define SENSOR_DEVICE_ATTR(_name, _mode, _show, _store, _index)	\
struct sensor_device_attribute sensor_dev_attr_##_name		\
	= SENSOR_ATTR(_name, _mode, _show, _store, _index)

#define SENSOR_DEVICE_ATTR_RO(_name, _func, _index)		\
	SENSOR_DEVICE_ATTR(_name, 0444, _func##_show, NULL, _index)

#define SENSOR_DEVICE_ATTR_RW(_name, _func, _index)		\
	SENSOR_DEVICE_ATTR(_name, 0644, _func##_show, _func##_store, _index)

#define SENSOR_DEVICE_ATTR_WO(_name, _func, _index)		\
	SENSOR_DEVICE_ATTR(_name, 0200, NULL, _func##_store, _index)

#endif /* _LINUX_HWMON_SYSFS_H */
