/* linuxu: SHIM (third_party/linux/include/linux/dma-mapping.h)
 *
 * The host test backend uses identity DMA addresses. The DriverKit backend
 * maps streaming buffers through an IODMACommand-backed bounce buffer and
 * returns a DART IOVA. Struct layouts (sg_table / dma_buf /
 * dma_resv) are defined here or in <linux/scatterlist.h> /
 * <linux/dma-buf.h>.
 */
#ifndef _LINUX_DMA_MAPPING_H
#define _LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/dma-addr.h>
#include <linux/dma-direction.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/gfp.h>

/* struct page: declared (NOT #include'd) — the dma-mapping and
 * scatterlist headers are mutually referential and neither may drag
 * in <linux/mm.h>; a forward declaration is all either needs for
 * `struct page *` parameters.  <linux/mm.h> stays the canonical
 * definition provider for the rest of the tree. */
struct page;
struct device;
struct scatterlist;
struct sg_table;

/* struct dma_map_ops — layout mirrors vendor linux/dma-map-ops.h (the
 * shim subset used by linuxu/src/drm/ttm.c's linuxu_dma_ops and the
 * dev->dma_ops field in linux/device.h). */
struct dma_map_ops {
	void *(*alloc)(struct device *dev, size_t size,
			dma_addr_t *dma_handle, gfp_t gfp,
			unsigned long attrs);
	void (*free)(struct device *dev, size_t size, void *vaddr,
			dma_addr_t dma_handle, unsigned long attrs);
	int (*get_sgtable)(struct device *dev, struct sg_table *sgt,
			void *cpu_addr, dma_addr_t dma_addr, size_t size,
			unsigned long attrs);
	dma_addr_t (*map_phys)(struct device *dev, phys_addr_t phys,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs);
	void (*unmap_phys)(struct device *dev, dma_addr_t dma_handle,
			size_t size, enum dma_data_direction dir,
			unsigned long attrs);
	int (*map_sg)(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs);
	void (*unmap_sg)(struct device *dev, struct scatterlist *sg, int nents,
			enum dma_data_direction dir, unsigned long attrs);
};


/* vendor 2026 dma_mapping_attr flags (subset used by the driver) */
#define DMA_ATTR_SKIP_CPU_SYNC	BIT(0)
#define DMA_ATTR_NO_KERNEL_MAPPING	BIT(1)
#define DMA_ATTR_NO_WARN		BIT(2)
#define DMA_ATTR_FORCE_CONTIGUOUS	BIT(3)

#define DMA_MAPPING_ERROR (~(dma_addr_t)0)

/* An error-encoded dma_addr_t has a value in the highest address page. */
static inline int dma_mapping_error(struct device *dev, dma_addr_t bus)
{
	(void)dev;
	return (bus >= (dma_addr_t)(uintptr_t)(-4095)) ? (int)(uintptr_t)bus : 0;
}

/* attrs variants used by ttm_pool.c (attrs forwarded, no-op) */
extern void *dma_alloc_attrs(struct device *dev, size_t size,
			     dma_addr_t *dma_handle, gfp_t flag,
			     unsigned long attrs);
extern void dma_free_attrs(struct device *dev, size_t size, void *vaddr,
			   dma_addr_t dma_handle, unsigned long attrs);

/* ---- core map/unmap (host DART layer: real token + mapped-byte accounting,
 * see rt/dart.h + linuxu/src/dart/dart.c) ---- */
extern dma_addr_t linuxu_dma_map_page(struct device *dev, struct page *page,
				      unsigned long offset, size_t size,
				      enum dma_data_direction dir);
extern void linuxu_dma_unmap_page(struct device *dev, dma_addr_t iova,
				  size_t size, enum dma_data_direction dir);
extern int linuxu_dma_map_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir);
extern void linuxu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
				 int nents, enum dma_data_direction dir);
extern void *linuxu_dma_alloc_coherent(struct device *dev, size_t size,
				       dma_addr_t *dma_handle, gfp_t gfp);
extern void linuxu_dma_free_coherent(struct device *dev, size_t size,
				      void *vaddr, dma_addr_t dma_handle);
#ifdef LINUXU_DEXT_DK
extern dma_addr_t linuxu_dma_map_single(struct device *dev, void *addr,
					 size_t size, enum dma_data_direction dir);
extern void linuxu_dma_unmap_single(struct device *dev, dma_addr_t address,
				     size_t size, enum dma_data_direction dir);
extern void linuxu_dma_sync_single_for_cpu(struct device *dev, dma_addr_t address,
					   size_t size, enum dma_data_direction dir);
extern void linuxu_dma_sync_single_for_device(struct device *dev, dma_addr_t address,
					      size_t size, enum dma_data_direction dir);
extern void *linuxu_dma_cpu_address(dma_addr_t address);
extern void *linuxu_sg_cpu_address(struct scatterlist *sg);
extern int linuxu_dma_map_sgtable(struct device *dev, struct sg_table *sgt,
				  enum dma_data_direction dir);
extern void linuxu_dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
				     enum dma_data_direction dir);
#endif



/* ---- core map/unmap ---- */
static inline dma_addr_t dma_map_resource(struct device *dev,
					  phys_addr_t phys_addr, size_t size,
					  enum dma_data_direction dir,
					  unsigned long attrs)
{
	(void)dev; (void)phys_addr; (void)size; (void)attrs; (void)dir;
	/* A PCI resource address is not a DART IOVA. Peer resource DMA
	 * needs a platform mapping operation that is not implemented. */
	return DMA_MAPPING_ERROR;
}

static inline dma_addr_t dma_map_single(struct device *dev, void *addr,
					size_t size,
					enum dma_data_direction dir)
{
#ifdef LINUXU_DEXT_DK
	return linuxu_dma_map_single(dev, addr, size, dir);
#else
	(void)dev; (void)size; (void)dir;
	return (dma_addr_t)(uintptr_t)addr;
#endif
}

static inline void dma_unmap_resource(struct device *dev,
				      dma_addr_t dma_addr, size_t size,
				      enum dma_data_direction dir,
				      unsigned long attrs)
{
	(void)dev; (void)dma_addr; (void)size; (void)dir; (void)attrs;
}

static inline void dma_unmap_single(struct device *dev, dma_addr_t dma_addr,
				    size_t size,
				    enum dma_data_direction dir)
{
#ifdef LINUXU_DEXT_DK
	linuxu_dma_unmap_single(dev, dma_addr, size, dir);
#else
	(void)dev; (void)dma_addr; (void)size; (void)dir;
#endif
}

static inline void dma_sync_single_for_cpu(struct device *dev,
					   dma_addr_t dma_handle, size_t size,
					   enum dma_data_direction dir)
{
#ifdef LINUXU_DEXT_DK
	linuxu_dma_sync_single_for_cpu(dev, dma_handle, size, dir);
#else
	(void)dev; (void)dma_handle; (void)size; (void)dir;
#endif
}

static inline void dma_sync_single_for_device(struct device *dev,
					      dma_addr_t dma_handle, size_t size,
					      enum dma_data_direction dir)
{
#ifdef LINUXU_DEXT_DK
	linuxu_dma_sync_single_for_device(dev, dma_handle, size, dir);
#else
	(void)dev; (void)dma_handle; (void)size; (void)dir;
#endif
}

/* ---- sg table map/unmap ---- */
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
				  enum dma_data_direction dir, unsigned int attrs)
{
#ifdef LINUXU_DEXT_DK
	(void)attrs;
	return linuxu_dma_map_sgtable(dev, sgt, dir);
#else
	(void)dev; (void)sgt; (void)dir; (void)attrs;
	return 0;
#endif
}

static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
				     enum dma_data_direction dir,
				     unsigned int attrs)
{
#ifdef LINUXU_DEXT_DK
	(void)attrs;
	linuxu_dma_unmap_sgtable(dev, sgt, dir);
#else
	(void)dev; (void)sgt; (void)dir; (void)attrs;
#endif
}

/* The device header includes this header before defining struct device;
 * implementations that inspect the pointer mask live in dma_mask.c. */
int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
int dma_set_mask_and_coherent(struct device *dev, u64 mask);
bool dma_supported(struct device *dev, u64 mask, int *align);
u64 dma_get_mask(struct device *dev);

/* ---- page map (host DART: real IOVA token + accounting) ---- */
static inline dma_addr_t dma_map_page(struct device *dev, struct page *page,
				      unsigned long offset, size_t size,
				      enum dma_data_direction dir)
{
	return linuxu_dma_map_page(dev, page, offset, size, dir);
}

static inline void dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
				  size_t size, enum dma_data_direction dir)
{
	linuxu_dma_unmap_page(dev, dma_handle, size, dir);
}

/* ---- sg map/unmap (host DART: per-entry IOVA + accounting) ---- */
static inline int dma_map_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir,
			      unsigned long attrs)
{
	(void)attrs;
	return linuxu_dma_map_sg(dev, sg, nents, dir);
}

static inline void dma_unmap_sg(struct device *dev, struct scatterlist *sg,
				 int nents, enum dma_data_direction dir,
				 unsigned long attrs)
{
	(void)attrs;
	linuxu_dma_unmap_sg(dev, sg, nents, dir);
}

static inline int dma_sync_sgtable_for_cpu(struct device *dev,
					   struct sg_table *sgt,
					   enum dma_data_direction dir)
{
	(void)dev; (void)sgt; (void)dir;
	return 0;
}

static inline int dma_sync_sgtable_for_device(struct device *dev,
					      struct sg_table *sgt,
					      enum dma_data_direction dir)
{
	(void)dev; (void)sgt; (void)dir;
	return 0;
}

static inline dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page,
						unsigned long offset, size_t size,
						enum dma_data_direction dir,
						unsigned long attrs)
{
	(void)attrs;
	return dma_map_page(dev, page, offset, size, dir);
}

static inline void dma_unmap_page_attrs(struct device *dev, dma_addr_t dma_handle,
					   size_t size, enum dma_data_direction dir,
					   unsigned long attrs)
{
	(void)attrs;
	dma_unmap_page(dev, dma_handle, size, dir);
}

static inline void *dma_alloc_coherent(struct device *dev, size_t size,
					dma_addr_t *dma_handle, gfp_t gfp)
{
	return linuxu_dma_alloc_coherent(dev, size, dma_handle, gfp);
}

static inline void dma_free_coherent(struct device *dev, size_t size,
				      void *vaddr, dma_addr_t dma_handle)
{
	linuxu_dma_free_coherent(dev, size, vaddr, dma_handle);
}

static inline void dma_set_max_seg_size(struct device *dev, unsigned int max)
{
	(void)dev; (void)max;
}
bool dma_addressing_limited(struct device *dev);
static inline unsigned long dma_max_mapping_size(struct device *dev)
{
	(void)dev;
	return (1UL << 40);
}

#endif /* _LINUX_DMA_MAPPING_H */
