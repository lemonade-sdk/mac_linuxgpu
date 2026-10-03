/* linuxu: SHIM (third_party/linux/include/linux/property.h) */
#ifndef __LINUX_PROPERTY_H
#define __LINUX_PROPERTY_H
#include <linux/types.h>
#include <linux/errno.h>
struct device;
/* No firmware-node property provider exists in the DriverKit device model. */
static inline int device_property_read_u32(struct device *dev, const char *prop, u32 *val)
{ (void)dev; return !prop || !val ? -EINVAL : -ENXIO; }
static inline int device_property_read_string(struct device *dev, const char *prop, const char **val)
{ (void)dev; return !prop || !val ? -EINVAL : -ENXIO; }
#endif
