#include <assert.h>
#include <stdbool.h>
#include <linux/pm_runtime.h>
#include <linux/pm.h>
#include <linux/errno.h>
#include <video/nomodeset.h>

int main(void)
{
	assert(!console_suspend_enabled);
	assert(!video_firmware_drivers_only());
	pm_runtime_allow(0);
	pm_runtime_forbid(0);
	pm_runtime_use_autosuspend(0);
	pm_runtime_set_autosuspend_delay(0, 5000);
	assert(pm_runtime_get(0) == 1);
	assert(pm_runtime_get_sync(0) == 1);
	assert(pm_runtime_resume(0) == 1);
	assert(pm_runtime_resume_and_get(0) == 0);
	assert(pm_runtime_get_if_active(0) == -EINVAL);
	assert(pm_runtime_get_if_in_use(0) == -EINVAL);
	assert(pm_runtime_active(0) && !pm_runtime_suspended(0));
	assert(!pm_runtime_enabled(0) && pm_runtime_blocked(0));
	assert(pm_runtime_block_if_disabled(0));
	assert(pm_runtime_suspend(0) == -ENOSYS);
	assert(pm_runtime_autosuspend(0) == -ENOSYS);
	assert(pm_runtime_idle(0) == -ENOSYS);
	assert(pm_runtime_put_autosuspend(0) == -ENOSYS);
	assert(pm_runtime_autosuspend_expiration(0) == 0);
	assert(pm_runtime_suspended_time(0) == 0);
	pm_runtime_get_noresume(0);
	pm_runtime_put_noidle(0);
	pm_runtime_put(0);
	return 0;
}
