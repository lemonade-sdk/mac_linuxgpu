/* linuxu: SHIM (third_party/linux/include/linux/pm_runtime.h)
 *
 * pm_runtime API surface. pm_runtime is NOOP:
 * the device never suspends in the dext. Runtime implementations follow
 * Linux's !CONFIG_PM contract in linuxu/src/shims/platform_policy.c.
 */
#ifndef __LINUX_PM_RUNTIME_H
#define __LINUX_PM_RUNTIME_H

#include <linux/types.h>

struct device;
struct device_link;

#define RPM_ENABLED		0x00000001
#define RPM_ACTIVE		0x00000002
#define RPM_AUTOSUSPEND		0x00000004
#define RPM_FORBIDDEN		0x00000008
#define RPM_DISABLE		0x00000010
#define RPM_AUTORESOURCE	0x00000020
#define RPM_ASYNC		0x00000040
#define RPM_SUSPENDED		0x00000080
#define RPM_RESUMING		0x00000100
#define RPM_RESUME			0x00000200
#define RPM_IDLE			0x00000400
#define RPM_SUSPEND			0x00000800

#define PM_RUNTIME_AUTO_SUSPEND_DELAY_MS	0

/* ---- runtime (linuxu/src/sync/pm_runtime.c) ---- */
extern int  pm_runtime_get(struct device *dev);
extern int  pm_runtime_get_sync(struct device *dev);
extern void pm_runtime_get_noresume(struct device *dev);
extern int  pm_runtime_get_if_active(struct device *dev);
extern int  pm_runtime_get_if_in_use(struct device *dev);
extern void pm_runtime_put(struct device *dev);
extern void pm_runtime_put_noidle(struct device *dev);
extern int  pm_runtime_put_autosuspend(struct device *dev);
extern int  pm_runtime_resume(struct device *dev);
extern int  pm_runtime_force_suspend(struct device *dev);
extern int  pm_runtime_force_resume(struct device *dev);
extern int  pm_runtime_suspend(struct device *dev);
extern int  pm_runtime_resume_and_get(struct device *dev);
extern int  pm_runtime_idle(struct device *dev);
extern void pm_runtime_enable(struct device *dev);
extern void pm_runtime_disable(struct device *dev);
extern void pm_runtime_allow(struct device *dev);
extern void pm_runtime_forbid(struct device *dev);
extern void pm_runtime_no_callbacks(struct device *dev);
extern void pm_runtime_irq_safe(struct device *dev);
extern void pm_runtime_barrier(struct device *dev);
extern bool pm_runtime_block_if_disabled(struct device *dev);
extern void pm_runtime_unblock(struct device *dev);
extern void pm_runtime_use_autosuspend(struct device *dev);
extern void pm_runtime_set_autosuspend_delay(struct device *dev, int delay);
extern void pm_runtime_set_memalloc_noio(struct device *dev, bool enable);
extern u64  pm_runtime_autosuspend_expiration(struct device *dev);
extern u64  pm_runtime_suspended_time(struct device *dev);
extern void pm_runtime_mark_last_busy(struct device *dev);

static inline bool pm_runtime_suspended(struct device *dev) { return false; }
static inline bool pm_runtime_active(struct device *dev) { return true; }
static inline bool pm_runtime_status_suspended(struct device *dev) { return false; }
static inline bool pm_runtime_enabled(struct device *dev) { return false; }
static inline bool pm_runtime_blocked(struct device *dev) { return true; }
static inline bool pm_runtime_has_no_callbacks(struct device *dev) { return false; }
static inline bool pm_runtime_is_irq_safe(struct device *dev) { return false; }

/* devm variant (used by some drivers) */
extern int devm_pm_runtime_enable(struct device *dev);

#endif /* __LINUX_PM_RUNTIME_H */
