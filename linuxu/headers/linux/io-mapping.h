/* I/O mappings retain a checked platform BAR mapping, never heap backing. */
#ifndef _LINUX_IO_MAPPING_H
#define _LINUX_IO_MAPPING_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/io.h>
#include <linux/mm.h>

/* pgprot_t comes from mm.h (single definition across the shim tree). */

struct io_mapping {
	resource_size_t base;
	unsigned long size;
	pgprot_t prot;
	void *iomem;
};

static inline struct io_mapping *
io_mapping_init_wc(struct io_mapping *iomap,
		   resource_size_t base,
		   unsigned long size)
{
	if (!iomap || !size || base > (resource_size_t)-1 - (size - 1))
		return NULL;
	void *address = ioremap_wc(base, size);
	if (!address) return NULL;
	iomap->base = base;
	iomap->size = size;
	iomap->prot = PAGE_WRITECOMBINE;
	iomap->iomem = address;
	return iomap;
}

static inline void
io_mapping_fini(struct io_mapping *mapping)
{
	if (!mapping) return;
	if (mapping->iomem) iounmap(mapping->iomem);
	mapping->iomem = NULL;
	mapping->size = 0;
}

/* Subranges share the mapping lease until io_mapping_fini(). */
static inline void *
io_mapping_map_wc(struct io_mapping *mapping,
		  unsigned long offset,
		  unsigned long size)
{
	if (!mapping || !mapping->iomem || !size ||
	    offset >= mapping->size || size > mapping->size - offset)
		return NULL;
	return (char *)mapping->iomem + offset;
}

static inline void
io_mapping_unmap(void *vaddr)
{
}

/* The shim has no CPU migration; each local mapping covers one host page. */
static inline void *
io_mapping_map_atomic_wc(struct io_mapping *mapping,
			 unsigned long offset)
{
	if (offset & (PAGE_SIZE - 1)) return NULL;
	return io_mapping_map_wc(mapping, offset, PAGE_SIZE);
}

static inline void
io_mapping_unmap_atomic(void *vaddr)
{
}

static inline void *
io_mapping_map_local_wc(struct io_mapping *mapping, unsigned long offset)
{
	if (offset & (PAGE_SIZE - 1)) return NULL;
	return io_mapping_map_wc(mapping, offset, PAGE_SIZE);
}

static inline void io_mapping_unmap_local(void *vaddr)
{
}

static inline struct io_mapping *
io_mapping_create_wc(resource_size_t base,
		     unsigned long size)
{
	struct io_mapping *iomap;

	if (!size)
		return NULL;

	iomap = kzalloc_obj(*iomap);
	if (!iomap)
		return NULL;

	if (!io_mapping_init_wc(iomap, base, size)) {
		kfree(iomap);
		return NULL;
	}
	return iomap;
}

static inline void
io_mapping_free(struct io_mapping *iomap)
{
	io_mapping_fini(iomap);
	kfree(iomap);
}

#endif /* _LINUX_IO_MAPPING_H */
