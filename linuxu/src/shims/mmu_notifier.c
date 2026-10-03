/* Interval notifier lifetime is implemented with tracked pages in mm/hmm.c. */
#include <linux/mmu_notifier.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>

/* ---- dev-backed iommu helpers (DART is the IOMMU) ---- */
struct device *mmu_get_domain_for_dev(struct device *dev)
{
	return dev; /* singleton domain */
}

void *mmu_iova_to_phys(struct device *dev, dma_addr_t iova)
{
	(void)dev;
#ifdef LINUXU_DEXT_DK
	return linuxu_dma_cpu_address(iova);
#else
	(void)iova;
	return NULL;
#endif
}

/* The pid lifecycle (get_task_pid, get_pid_task, put_pid) lives with the
 * task identity in task.c. */

/* aperture conflict removal (in-process: no legacy VGA framebuffer) */
#include <linux/aperture.h>
int aperture_remove_conflicting_devices(resource_size_t base,
					resource_size_t size,
					resource_size_t align,
					const char *name)
{
	return 0;
}
int aperture_remove_legacy_vga_devices(struct pci_dev *pdev,
				       resource_size_t size)
{
	return 0;
}
int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev,
					    const char *name)
{
	return 0;
}
int aperture_remove_all_conflicting_devices(const char *name)
{
	return 0;
}
