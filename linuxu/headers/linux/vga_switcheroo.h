/* linuxu: SHIM (third_party/linux/include/linux/vga_switcheroo.h)
 * Structs/enums verbatim from the pinned 2026 vendor header; the driver
 * (amdgpu_device.c) is the ABI test. The shim has no vga_switcheroo
 * core, so the runtime API is a no-op set (host: single GPU, iGPU handoff
 * not exercised). */
#ifndef __LINUX_VGA_SWITCHEROO_H
#define __LINUX_VGA_SWITCHEROO_H

#include <linux/types.h>

struct pci_dev;
struct device;
struct pci_dev;
struct dev_pm_domain;
struct fb_info;

/* Client power state. */
enum vga_switcheroo_state {
	VGA_SWITCHEROO_OFF,
	VGA_SWITCHEROO_ON,
	VGA_SWITCHEROO_NOT_FOUND,
};

enum vga_switcheroo_client_id {
	VGA_SWITCHEROO_UNKNOWN,
	VGA_SWITCHEROO_I915,
	VGA_SWITCHEROO_INTEL_AUDIO,
	VGA_SWITCHEROO_AMDGPU,
	VGA_SWITCHEROO_AMD_AUDIO,
	VGA_SWITCHEROO_ASUS_WMI,
};

struct vga_switcheroo_handler;

/* struct vga_switcheroo_client_ops (vendor 2026, verbatim) */
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, enum vga_switcheroo_client_id);
};

/* ---- no-op runtime (no vga_switcheroo core in the shim) ---- */
static inline void vga_switcheroo_unregister_client(struct pci_dev *dev)
{
	(void)dev;
}
static inline int vga_switcheroo_register_client(struct pci_dev *dev,
		const struct vga_switcheroo_client_ops *ops,
		bool driver_power_control)
{
	(void)dev; (void)ops; (void)driver_power_control;
	return 0;
}
static inline int vga_switcheroo_register_audio_client(struct pci_dev *pdev,
	const struct vga_switcheroo_client_ops *ops,
	const struct pci_dev *vga_dev)
{
	(void)pdev; (void)ops; (void)vga_dev;
	return 0;
}
static inline void vga_switcheroo_client_fb_set(struct pci_dev *dev,
		struct fb_info *info)
{
	(void)dev; (void)info;
}
static inline int vga_switcheroo_lock_ddc(struct pci_dev *pdev)
{
	(void)pdev;
	return -19; /* -ENODEV */
}
static inline int vga_switcheroo_unlock_ddc(struct pci_dev *pdev)
{
	(void)pdev;
	return -19; /* -ENODEV */
}
static inline int vga_switcheroo_process_delayed_switch(void)
{
	return 0;
}
static inline bool vga_switcheroo_client_probe_defer(struct pci_dev *pdev)
{
	(void)pdev;
	return false;
}
static inline enum vga_switcheroo_state
vga_switcheroo_get_client_state(struct pci_dev *dev)
{
	(void)dev;
	return VGA_SWITCHEROO_ON;
}
static inline int vga_switcheroo_init_domain_pm_ops(struct device *dev,
		struct dev_pm_domain *domain)
{
	(void)dev; (void)domain;
	return 0;
}
static inline void vga_switcheroo_fini_domain_pm_ops(struct device *dev)
{
	(void)dev;
}

static inline bool video_is_primary_device(struct device *dev)
{
	(void)dev;
	return true;
}
extern int vga_client_register(struct pci_dev *pdev,
			       unsigned int (*set_decode)(struct pci_dev *pdev, bool state));
extern void vga_client_unregister(struct pci_dev *pdev);

#endif /* __LINUX_VGA_SWITCHEROO_H */
