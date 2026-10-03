/* linuxu: SHIM (vendor/linux/include/linux/hwmon.h)
 *
 * Minimal hwmon surface for amdgpu pm (amdgpu_pm.c): only
 * hwmon_device_register_with_groups()/hwmon_device_unregister() and
 * the channel type enum are used.
 */
#ifndef __LINUX_HWMON_H
#define __LINUX_HWMON_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>

struct attribute_group;

enum hwmon_chip_info_flags {
	HWMON_IS_REALSensor		= 1,
	HWMON_IS_REGISTER		= 2,
	HWMON_IS_REGISTER_RAW		= 4,
	HWMON_IS_VIRTUAL		= 8,
	HWMON_IS_VIRTUAL_FROM_REG	= 16,
};

/* hwmon channel types (subset, values from upstream hwmon.h) */
enum hwmon_sensor_types {
	hwmon_chip,
	hwmon_temp,
	hwmon_fan,
	hwmon_pwm,
	hwmon_in,
	hwmon_power,
	hwmon_curr,
	hwmon_energy,
	hwmon_freq,
	hwmon_alrm,
	hwmon_als,
	hwmon_cpu,
	hwmon_gpu,
};

extern struct device *hwmon_device_register_with_groups(struct device *dev,
			const char *name, void *drvdata,
			const struct attribute_group **groups);
extern void hwmon_device_unregister(struct device *dev);

#endif /* __LINUX_HWMON_H */
