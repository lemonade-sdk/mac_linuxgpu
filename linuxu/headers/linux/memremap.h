/* linuxu: SHIM (third_party/linux/include/linux/memremap.h)
 *
 * ZONE_DEVICE (dev_pagemap) API surface. Used by kfd_migrate.c
 * (SVM private memory) and amdgpu_amdkfd.h. Runtime:
 * linuxu/src/mm/memremap.c.
 */
#include <linux/mm.h>

#ifndef __LINUX_MEMREMAP_H
#define __LINUX_MEMREMAP_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>
struct vm_fault;
typedef unsigned int vm_fault_t_shim;

#include <linux/device.h>
#include <linux/completion.h>

/* MEMREMAP_* caching flags (values match asm-generic/io.h) */
#define MEMREMAP_WB	(1UL << 0)
#define MEMREMAP_WT	(1UL << 1)
#define MEMREMAP_WC	(1UL << 2)
#define MEMREMAP_DEFAULT	0UL

struct range {
	resource_size_t start;
	resource_size_t end;
};

struct vmem_altmap {
	resource_size_t start;
	resource_size_t size;
	unsigned long   alloced;
	void           *va;
};

enum memory_type {
	MEMORY_DEVICE_PRIVATE = 1,
	MEMORY_DEVICE_COHERENT = 2,
	MEMORY_DEVICE_WB = 3,
	MEMORY_DEVICE_PCI_P2PDMA = 4,
};

#define PGMAP_ALTMAP_VALID	(1 << 0)
#define PGMAP_OWNER		(1 << 1)

struct folio;

struct dev_pagemap_ops {
	int (*memory_failure)(struct dev_pagemap *pgmap, unsigned long pfn,
			      unsigned long nr_pages, int mf_flags);
	void (*folio_free)(struct folio *folio);
	vm_fault_t_shim (*migrate_to_ram)(struct vm_fault *vmf);
};

struct percpu_ref {
	atomic_long_t count;
};

struct dev_pagemap {
	struct vmem_altmap altmap;
	struct percpu_ref ref;
	struct completion done;
	enum memory_type type;
	unsigned int flags;
	unsigned long vmemmap_shift;
	const struct dev_pagemap_ops *ops;
	void *owner;
	int nr_range;
	union {
		struct range range;
		struct range ranges[1];
	};
};

static inline bool pgmap_has_memory_failure(struct dev_pagemap *pgmap)
{
	return pgmap->ops && pgmap->ops->memory_failure;
}

static inline struct vmem_altmap *pgmap_altmap(struct dev_pagemap *pgmap)
{
	if (pgmap->flags & PGMAP_ALTMAP_VALID)
		return &pgmap->altmap;
	return NULL;
}

static inline unsigned long pgmap_vmemmap_nr(struct dev_pagemap *pgmap)
{
	return 1 << pgmap->vmemmap_shift;
}

/* ---- runtime (linuxu/src/mm/memremap.c) ---- */
extern void *memremap_pages(struct dev_pagemap *pgmap, int nid);
extern void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap);
extern void zone_device_page_init(struct page *page, struct dev_pagemap *pgmap,
				  unsigned long pfn);
extern bool is_zone_device_page(const struct page *page);
extern struct dev_pagemap *page_pgmap(const struct page *page);

#endif /* __LINUX_MEMREMAP_H */
