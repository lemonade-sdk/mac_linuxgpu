#include <assert.h>
#include <stdio.h>
#include <linux/suspend.h>
#include <linux/suspend.h>

#ifdef CONFIG_PM_SLEEP
#error "This regression requires the production CONFIG_PM_SLEEP=n policy"
#endif

_Static_assert(__builtin_types_compatible_p(__typeof__(&unregister_pm_notifier),
	int (*)(struct notifier_block *)), "upstream unregister return type");

struct amdgpu_device {
	struct notifier_block pm_nb;
};

static unsigned int callback_calls, failed_init_calls;

static int amdgpu_device_pm_notifier(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	(void)nb;
	(void)event;
	(void)data;
	callback_calls++;
	return NOTIFY_OK;
}

#ifdef REPRODUCE_OLD_PM_NOTIFIER_STUB
/* The old generated stub made upstream's final init step fail with -38. */
static int legacy_register_pm_notifier(struct notifier_block *nb)
{
	(void)nb;
	return -ENOSYS;
}
#define register_pm_notifier legacy_register_pm_notifier
#endif

static int finish_device_init(struct amdgpu_device *adev)
{
	int r;
/* Unchanged final notifier registration/return block from amdgpu_device_init. */
#include "amdgpu_pm_init_tail.inc"
failed:
	failed_init_calls++;
	return r;
}

int main(void)
{
	struct notifier_block sentinel = { 0 };
	struct amdgpu_device adev = { .pm_nb = {
		.next = &sentinel, .priority = 17,
	} };
	int status = finish_device_init(&adev);
#ifdef REPRODUCE_OLD_PM_NOTIFIER_STUB
	assert(status == -ENOSYS && failed_init_calls == 1);
	puts("Reproduced upstream initialization failure -38 with the old PM stub");
#else
	assert(status == 0 && failed_init_calls == 0);
	assert(unregister_pm_notifier(&adev.pm_nb) == 0);
	assert(adev.pm_nb.next == &sentinel && adev.pm_nb.priority == 17);
	assert(callback_calls == 0);
	assert(pm_sleep_ptr(amdgpu_device_pm_notifier) == NULL);
	assert(pm_ptr(amdgpu_device_pm_notifier) == amdgpu_device_pm_notifier);
	puts("Upstream initialization tail passes with Linux system sleep disabled");
#endif
	return 0;
}
