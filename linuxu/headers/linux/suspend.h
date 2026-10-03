/* linuxu: SHIM (third_party/linux/include/linux/suspend.h)
 *
 * Linux system sleep is disabled. Match the upstream !CONFIG_PM_SLEEP
 * notifier contract; this does not implement macOS sleep/resume handling.
 */
#ifndef _LINUX_SUSPEND_H
#define _LINUX_SUSPEND_H

#include <linux/types.h>
#include <linux/pm.h>
#include <linux/notifier.h>

/* suspend state (vendor 2026 suspend.h) — device.h typedefs this as int
 * in the shim; keep both legal by only defining the enum values. */
#ifndef PM_SUSPEND_ON
enum {
	PM_SUSPEND_ON = 0,
	PM_SUSPEND_STANDBY,
	PM_SUSPEND_MEM,
	PM_SUSPEND_MAX
};
#endif

/* CONFIG_SUSPEND=n on the shim: pm_suspend_target_state is the macro form */
#define pm_suspend_target_state	(PM_SUSPEND_ON)

static inline void pm_set_vt_switch(int do_switch)
{
	(void)do_switch;
}

static inline void pm_prepare_console(void)
{
}

static inline void pm_restore_console(void)
{
}

#ifdef CONFIG_PM_SLEEP
extern int register_pm_notifier(struct notifier_block *nb);
extern int unregister_pm_notifier(struct notifier_block *nb);
#else
static inline int register_pm_notifier(struct notifier_block *nb)
{
	(void)nb;
	return 0;
}

static inline int unregister_pm_notifier(struct notifier_block *nb)
{
	(void)nb;
	return 0;
}
#endif
extern int system_state;

/* Hibernation and suspend events from the pinned Linux header. */
#define PM_HIBERNATION_PREPARE 0x0001
#define PM_POST_HIBERNATION    0x0002
#define PM_SUSPEND_PREPARE     0x0003
#define PM_POST_SUSPEND        0x0004
#define PM_RESTORE_PREPARE     0x0005
#define PM_POST_RESTORE       0x0006

#endif /* _LINUX_SUSPEND_H */
