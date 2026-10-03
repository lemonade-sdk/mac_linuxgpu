/* linuxu shim: acpi — ACPI no-ops;
 * (CONFIG_ACPI=n: the amdgpu_acpi.c path compiles to stubs). */
#include <linux/types.h>

/* acpi_os_* and the handful of amdgpu_acpi entry points resolve to
 * no-ops; acpi_get_handle etc. report "not present". */

int acpi_is_present(void)
{
	return 0;
}

void acpi_set_pdc_function(void *func)
{
	(void)func;
}

int acpi_os_execute(int type, void *func, void *arg)
{
	(void)type; (void)func; (void)arg;
	return -1; /* -AE_NOT_FOUND */
}

int acpi_os_wait_events_complete(void)
{
	return 0;
}
