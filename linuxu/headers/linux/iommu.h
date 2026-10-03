/* linuxu: SHIM (third_party/linux/include/linux/iommu.h) */
#ifndef __LINUX_IOMMU_H
#define __LINUX_IOMMU_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/errno.h>

struct iommu_domain {
	int type;
};
#define IOMMU_DOMAIN_BLOCKED 0U
#define IOMMU_DOMAIN_IDENTITY (1U << 2)
#define IOMMU_DOMAIN_DMA ((1U << 0) | (1U << 1))
#define IOMMU_DOMAIN_DMA_FQ (IOMMU_DOMAIN_DMA | (1U << 3))

static inline struct iommu_domain *iommu_get_domain_for_dev(
	struct device *dev)
{
#ifdef LINUXU_DEXT_DK
	/* DriverKit PCI DMA addresses are translated by DART. Reporting no
	 * domain would make AMDGPU treat IOVAs as host physical addresses. */
	static struct iommu_domain dart_domain = { .type = IOMMU_DOMAIN_DMA };
	return dev && dev->bus == &pci_bus_type ? &dart_domain : NULL;
#else
	(void)dev;
	return NULL;
#endif
}

static inline dma_addr_t iommu_iova_to_phys(struct iommu_domain *dom,
					    dma_addr_t iova)
{
	/* Physical addresses are not exposed by IODMACommand. Its DMA API
	 * mappings are owned by dart.c; reject diagnostic physical access. */
	return !dom || dom->type == IOMMU_DOMAIN_IDENTITY ? iova : (dma_addr_t)-1;
}

static inline int iommu_map(struct iommu_domain *dom, unsigned long iova,
			    phys_addr_t paddr, size_t size, int prot,
			    gfp_t gfp)
{
	(void)dom; (void)iova; (void)paddr; (void)size;
	(void)prot; (void)gfp;
	return -EOPNOTSUPP;
}

static inline size_t iommu_unmap(struct iommu_domain *dom,
				 unsigned long iova, size_t size)
{
	(void)dom; (void)iova; (void)size;
	return 0;
}

#endif /* __LINUX_IOMMU_H */
