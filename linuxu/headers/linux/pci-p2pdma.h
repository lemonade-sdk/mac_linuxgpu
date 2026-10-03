/* linuxu: SHIM (third_party/linux/include/linux/pci-p2pdma.h) */
#ifndef __LINUX_PCI_P2PDMA_H
#define __LINUX_PCI_P2PDMA_H

#include <linux/types.h>
#include <linux/pci.h>

struct pci_p2pdma_window;

static inline struct pci_p2pdma_window *pci_p2pdma_add_window(
	struct pci_dev *dev, phys_addr_t pci_start,
	phys_addr_t pci_end, size_t sys_align)
{
	(void)dev; (void)pci_start; (void)pci_end; (void)sys_align;
	return NULL;
}

static inline void pci_p2pdma_remove_window(
	struct pci_p2pdma_window *window, bool wait)
{
	(void)window; (void)wait;
}

static inline int pci_p2pdma_page_to_pfn(struct page *page)
{
	(void)page;
	return 0;
}

#endif /* __LINUX_PCI_P2PDMA_H */
