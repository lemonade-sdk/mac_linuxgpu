/* DriverKit platform policy where Linux console and runtime PM do not exist. */
#include <stdbool.h>
#include <linux/errno.h>
#include <linux/pm_runtime.h>
#include <video/nomodeset.h>

/* There is no Linux console to suspend in the dext. */
bool console_suspend_enabled = false;

/* This is a compute-only driver, but it is still allowed to bind the GPU. */
bool video_firmware_drivers_only(void)
{
	return false;
}

/* The runtime-PM module parameter is forced off before upstream amdgpu_init.
 * These policy calls cannot schedule autosuspend or change PCI power state. */
void pm_runtime_allow(struct device *dev) { (void)dev; }
void pm_runtime_forbid(struct device *dev) { (void)dev; }
void pm_runtime_use_autosuspend(struct device *dev) { (void)dev; }
void pm_runtime_set_autosuspend_delay(struct device *dev, int delay)
{
	(void)dev;
	(void)delay;
}

/* Match Linux's !CONFIG_PM behavior: resume is already satisfied, while
 * requesting an unsupported suspend never reports success. Native DRM/KFD
 * callers still use get_sync even when AMDGPU runtime PM is disabled. */
int pm_runtime_get(struct device *dev) { (void)dev; return 1; }
int pm_runtime_get_sync(struct device *dev) { (void)dev; return 1; }
int pm_runtime_resume(struct device *dev) { (void)dev; return 1; }
int pm_runtime_resume_and_get(struct device *dev) { (void)dev; return 0; }
void pm_runtime_get_noresume(struct device *dev) { (void)dev; }
int pm_runtime_get_if_active(struct device *dev) { (void)dev; return -EINVAL; }
int pm_runtime_get_if_in_use(struct device *dev) { (void)dev; return -EINVAL; }
void pm_runtime_put(struct device *dev) { (void)dev; }
void pm_runtime_put_noidle(struct device *dev) { (void)dev; }
int pm_runtime_put_autosuspend(struct device *dev) { (void)dev; return -ENOSYS; }
int pm_runtime_idle(struct device *dev) { (void)dev; return -ENOSYS; }
int pm_runtime_suspend(struct device *dev) { (void)dev; return -ENOSYS; }
int pm_runtime_force_suspend(struct device *dev) { (void)dev; return -ENOSYS; }
int pm_runtime_force_resume(struct device *dev) { (void)dev; return -ENXIO; }
void pm_runtime_enable(struct device *dev) { (void)dev; }
void pm_runtime_disable(struct device *dev) { (void)dev; }
void pm_runtime_no_callbacks(struct device *dev) { (void)dev; }
void pm_runtime_irq_safe(struct device *dev) { (void)dev; }
void pm_runtime_barrier(struct device *dev) { (void)dev; }
bool pm_runtime_block_if_disabled(struct device *dev) { (void)dev; return true; }
void pm_runtime_unblock(struct device *dev) { (void)dev; }
void pm_runtime_set_memalloc_noio(struct device *dev, bool enable) { (void)dev; (void)enable; }
u64 pm_runtime_autosuspend_expiration(struct device *dev) { (void)dev; return 0; }
u64 pm_runtime_suspended_time(struct device *dev) { (void)dev; return 0; }
void pm_runtime_mark_last_busy(struct device *dev) { (void)dev; }
int devm_pm_runtime_enable(struct device *dev) { (void)dev; return 0; }
