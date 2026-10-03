/* ZONE_DEVICE migration is unavailable without a registered VRAM page map.
 * These pointer-returning Linux APIs must report errors with ERR_PTR. */
#include <linux/memremap.h>
#include <linux/pci.h>   /* struct resource definition */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/errno.h>

void *memremap_pages(struct dev_pagemap *pgmap, int nid)
{
	(void)pgmap;
	(void)nid;
	return ERR_PTR(-EOPNOTSUPP);
}

void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap)
{
	(void)dev; (void)pgmap;
	return ERR_PTR(-EOPNOTSUPP);
}

/* memory-region bookkeeping (upstream linux/ioport.h); the shim model
 * has no real bus windows, so a fixed fake window is handed out. */
struct resource iomem_resource = {
	.start = 0,
	.end = ~0ULL,
	.name = "iomem",
	.flags = IORESOURCE_BUSY | IORESOURCE_MEM,
};

struct resource *devm_request_free_mem_region(struct device *dev,
					      struct resource *parent,
					      resource_size_t size)
{
	if (!dev || !parent || !size)
		return ERR_PTR(-EINVAL);
	/* A fabricated physical range would let KFD enable unsupported SVM. */
	return ERR_PTR(-EOPNOTSUPP);
}

void devm_release_mem_region(struct device *dev, resource_size_t start,
			     resource_size_t size)
{
	(void)dev; (void)start; (void)size;
}

void zone_device_page_init(struct page *page, struct dev_pagemap *pgmap,
			   unsigned long pfn)
{
	(void)page; (void)pgmap; (void)pfn;
	/* No page can reach this path through a successful memremap_pages. */
}

bool is_zone_device_page(const struct page *page)
{
	(void)page;
	return false; /* TTM uses private for order and DMA metadata. */
}

struct dev_pagemap *page_pgmap(const struct page *page)
{
	(void)page;
	return NULL;
}
