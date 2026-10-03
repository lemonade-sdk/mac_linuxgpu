/* linuxu: SHIM (third_party/linux/include/linux/apple-gmux.h)
 *
 * Apple gmux (MacBook dual-GPU) detection. The host is not a dual-GPU
 * MacBook, so CONFIG_APPLE_GMUX is off and the API resolves to the
 * no-op #else form of the vendor header (amdgpu_device.c calls
 * apple_gmux_detect(NULL, NULL)).
 */
#ifndef LINUX_APPLE_GMUX_H
#define LINUX_APPLE_GMUX_H

#include <linux/types.h>
#include <linux/acpi.h>

#define GMUX_ACPI_HID "APP000B"

enum apple_gmux_type {
	APPLE_GMUX_TYPE_PIO,
	APPLE_GMUX_TYPE_INDEXED,
	APPLE_GMUX_TYPE_MMIO,
};

struct pnp_dev;

static inline bool apple_gmux_present(void)
{
	return false;
}

static inline bool apple_gmux_detect(struct pnp_dev *pnp_dev, bool *indexed_ret)
{
	(void)pnp_dev;
	(void)indexed_ret;
	return false;
}

#endif /* LINUX_APPLE_GMUX_H */
