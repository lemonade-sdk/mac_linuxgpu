/* Link and exercise the actual pinned DRM, scheduler, KFD and AMDGPU init. */
#include <assert.h>
#include <unistd.h>
#include "mock_memory_sysctl.h"
#include <linux/array_size.h>
#include <linux/errno.h>
#include <rt/bootstrap.h>
#include <rt/rt.h>

static int array_size_contract[256];
_Static_assert(ARRAY_SIZE(array_size_contract) == 256,
		"ARRAY_SIZE must not advance past static tables");

int main(void)
{
	int ret;

	assert(rt_device_active_pdev() == NULL);
	ret = linuxu_driver_bootstrap();
	if (ret) {
		(void)write(2, "driver bootstrap failed\n", 24);
		return 1;
	}
	assert(rt_device_active_pdev() == NULL);
	assert(linuxu_driver_bootstrap() == -EALREADY);
	linuxu_driver_shutdown();
	assert(rt_device_active_pdev() == NULL);

	ret = linuxu_driver_bootstrap();
	if (ret) {
		(void)write(2, "driver bootstrap after shutdown failed\n", 39);
		return 2;
	}
	linuxu_driver_shutdown();
	(void)write(1, "upstream driver bootstrap and shutdown passed without PCI device\n", 65);
	return 0;
}
