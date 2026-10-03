/* linuxu: SHIM (third_party/linux/include/linux/vgaarb.h)
 *
 * VGA arbitration is a no-op on the host (single GPU, no VGA decode
 * conflicts). amdgpu_device.c calls vga_set_legacy_decoding() and
 * vga_get() / vga_put(); all resolve to the no-op #else form.
 */
#ifndef LINUX_VGA_H
#define LINUX_VGA_H

#include <linux/types.h>

struct pci_dev;

#define VGA_RSRC_NONE	       0x00
#define VGA_RSRC_LEGACY_IO     0x01
#define VGA_RSRC_LEGACY_MEM    0x02
#define VGA_RSRC_LEGACY_MASK   (VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define VGA_RSRC_NORMAL_IO     0x04
#define VGA_RSRC_NORMAL_MEM    0x08

static inline void vga_set_legacy_decoding(struct pci_dev *pdev, unsigned int decodes)
{
	(void)pdev; (void)decodes;
}

static inline int vga_get(struct pci_dev *pdev, unsigned int rsrc, int interruptible)
{
	(void)pdev; (void)rsrc; (void)interruptible;
	return 0;
}

static inline void vga_put(struct pci_dev *pdev, unsigned int rsrc)
{
	(void)pdev; (void)rsrc;
}

#endif /* LINUX_VGA_H */
