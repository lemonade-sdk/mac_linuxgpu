/* linuxu: SHIM (third_party/linux/include/linux/power_supply.h)
 *
 * Minimal surface for amdgpu pm: only the query helpers the KMD calls.
 * The full class model (struct power_supply/desc) is not used by the
 * driver's pm code.
 */
#ifndef __LINUX_POWER_SUPPLY_H
#define __LINUX_POWER_SUPPLY_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * power_supply_is_system_supplied - is the system running on system power?
 *
 * Values: 0 = on battery, 1 = on system power (AC/mains),
 *         negative errno if the power supply class is unavailable.
 */
static inline int power_supply_is_system_supplied(void)
{
	return 1;
}

#endif /* __LINUX_POWER_SUPPLY_H */
