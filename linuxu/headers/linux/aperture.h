/* linuxu: SHIM (third_party/linux/include/linux/aperture.h)
 *
 * Framebuffer aperture conflict resolution. In-process there is no
 * conflicting legacy VGA framebuffer, so all probes succeed.
 */
#ifndef _LINUX_APERTURE_H
#define _LINUX_APERTURE_H

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/types.h>

int aperture_remove_conflicting_devices(resource_size_t base,
					resource_size_t size,
					resource_size_t align,
					const char *name);
int aperture_remove_legacy_vga_devices(struct pci_dev *pdev,
				       resource_size_t size);
int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev,
					    const char *name);
int aperture_remove_all_conflicting_devices(const char *name);

static inline int devm_aperture_acquire_for_platform_device(
		struct platform_device *pdev, const char *name)
{
	return 0;
}

#endif /* _LINUX_APERTURE_H */
