/* linuxu: SHIM (third_party/linux/include/linux/backlight.h)
 *
 * Backlight device API for amdgpu (atombios_encoders.c / amdgpu_acpi.c /
 * amdgpu_dm.c). Aligned to the pinned vendor 2026 text for the struct
 * layout (backlight_ops / backlight_properties / backlight_device);
 * registration is a no-op (no sysfs class on the host).
 */
#ifndef _LINUX_BACKLIGHT_H
#define _LINUX_BACKLIGHT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/err.h>

/* ---- enum backlight_type (vendor 2026) ---- */
enum backlight_type {
	BACKLIGHT_RAW = 1,
	BACKLIGHT_PLATFORM,
	BACKLIGHT_FIRMWARE,
	BACKLIGHT_TYPE_MAX,
};

/* ---- struct backlight_properties (vendor 2026) ---- */
#define BACKLIGHT_POWER_ON		(0)
#define BACKLIGHT_POWER_OFF		(4)
#define BACKLIGHT_POWER_REDUCED		(1)
struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	enum backlight_type type;
	unsigned int state;
};
#define BL_CORE_SUSPENDED	(1 << 0)
#define BL_CORE_FBBLANK		(1 << 1)

/* ---- struct backlight_ops ---- */
struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
	bool (*controls_device)(struct backlight_device *bd,
				struct device *display_dev);
};

/* ---- struct backlight_device ---- */
struct backlight_device {
	struct backlight_properties props;
	struct mutex update_lock;
	struct mutex ops_lock;
	const struct backlight_ops *ops;
	struct list_head entry;
	struct device dev;
	int use_count;
};


#define to_backlight_device(obj) \
	container_of(obj, struct backlight_device, dev)

static inline void *bl_get_data(struct backlight_device *bl_dev)
{
	return dev_get_drvdata(&bl_dev->dev);
}

static inline int backlight_update_status(struct backlight_device *bd)
{
	if (bd && bd->ops && bd->ops->update_status)
		return bd->ops->update_status(bd);
	return 0;
}

/* vendor 2026 acpi video backlight preference (backlight.h); the host
 * has no ACPI video device, so native is always preferred. */
static inline bool acpi_video_backlight_use_native(void)
{
	return true;
}

extern struct backlight_device *backlight_device_register(
	const char *name, struct device *dev, void *devdata,
	const struct backlight_ops *ops,
	const struct backlight_properties *props);
extern void backlight_device_unregister(struct backlight_device *bd);

#endif /* _LINUX_BACKLIGHT_H */
