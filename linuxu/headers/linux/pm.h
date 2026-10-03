/* linuxu: SHIM (third_party/linux/include/linux/pm.h) */
#ifndef __LINUX_PM_H
#define __LINUX_PM_H
#include <linux/types.h>
#include <linux/errno.h>
struct device;
static inline bool pm_resume_via_firmware(void) { return false; }

#define DPM_FLAG_NO_DIRECT_COMPLETE  (1 << 0)
#define DPM_FLAG_SMART_PREPARE       (1 << 1)
#define DPM_FLAG_SMART_SUSPEND       (1 << 2)
#define DPM_FLAG_MAY_SKIP_RESUME     (1 << 3)

static inline void dev_pm_set_driver_flags(struct device *dev, int driver_flags)
{
	(void)dev; (void)driver_flags;
}

#define pm_ptr(x) (x)
#ifdef CONFIG_PM_SLEEP
#define pm_sleep_ptr(x) (x)
#else
#define pm_sleep_ptr(x) (0 ? (x) : NULL)
#endif
static inline int pm_runtime_autosuspend(struct device *dev)
{
	(void)dev;
	return -ENOSYS;
}
static inline bool pm_hibernate_is_recovering(void) { return false; }
static inline bool pm_hibernation_mode_is_suspend(void) { return true; }
extern bool console_suspend_enabled;
#define TAINT_CPU_OUT_OF_SPEC 256

/*
 * struct dev_pm_ops - device PM callbacks (vendor 2026 layout, verbatim).
 */
struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};
#endif
