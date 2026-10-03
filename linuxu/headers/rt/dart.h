/* linuxu: SHIM (DeviceKit runtime glue) */
/*
 * rt/dart.h — DMA→DART mapping layer. The host test backend uses identity
 * IOVAs. DriverKit coherent allocations use IODMACommand directly, while
 * streaming mappings use a DART-mapped bounce buffer with explicit copies at
 * DMA ownership boundaries. Both paths enforce the 1.5 GB live mapping
 * budget and report failures through dma_mapping_error().
 *
 * The token table behind the counter is a small hash of live mappings,
 * so double-unmap and unmap-of-unknown are detectable no-ops and
 * teardown is leak-checkable (linuxu_dart_table_count()).
 */
#ifndef LINUXU_RT_DART_H
#define LINUXU_RT_DART_H

#include <stddef.h>
#include <stdint.h>

#include <linux/types.h>
#include <linux/dma-addr.h>
#include <linux/dma-direction.h>
#include <linux/gfp.h>

/* struct declarations only: this header must stay includable without
 * <linux/mm.h> / <linux/device.h> (those headers' include DAGs are
 * owned by other tracks and change under it).  The canonical
 * declarations of the linuxu_dma_* family live in
 * <linux/dma-mapping.h> (with <linux/mm.h> providing struct page);
 * the prototypes below must agree with them. */
struct device;
struct page;
struct scatterlist;

#ifdef __cplusplus
extern "C" {
#endif

/* ~1.5 GB DART budget (matches rt_dart_ceiling in rt/rt.h). */
#define LINUXU_DART_BUDGET  (1536ULL * 1024ULL * 1024ULL)

/* dma_map_page equivalent over a struct page (backing = page host VA). */
dma_addr_t linuxu_dma_map_page(struct device *dev, struct page *page,
			       unsigned long offset, size_t size,
			       enum dma_data_direction dir);
void linuxu_dma_unmap_page(struct device *dev, dma_addr_t iova, size_t size,
			   enum dma_data_direction dir);

/* Scatterlist map/unmap: each entry gets a valid IOVA and the mapped bytes
 * are budgeted. Returns the number of mapped entries, 0 on failure. */
int linuxu_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
		      enum dma_data_direction dir);
void linuxu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			 int nents, enum dma_data_direction dir);

/* Real 16 KB-aligned anon allocation (firmware/ucode load depends on
 * this not returning NULL). */
void *linuxu_dma_alloc_coherent(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp);
void linuxu_dma_free_coherent(struct device *dev, size_t size, void *vaddr,
			      dma_addr_t dma_handle);

/* Whether [iova, iova + bytes) lies inside one live mapping: what the
 * IOMMU (DART) would translate for the device. An access outside every
 * live mapping is one the DART faults, and behind a Thunderbolt tunnel a
 * DART fault can take the device off the bus. */
int linuxu_dart_contains(uint64_t iova, uint64_t bytes);

/* Hold DMA releases while @stalled(@arg) reports that a GPU engine has
 * work it has been running longer than its job timeout: memory the driver
 * frees then stays mapped (and allocated) until no engine is stalled, so
 * late work cannot reach an IOVA the DART no longer maps or has handed out
 * again. NULL removes the predicate and releases what was held. Released
 * opportunistically by later mapping calls, or by
 * linuxu_dart_release_held (which returns how many releases are still
 * held). */
void linuxu_dart_set_hold(bool (*stalled)(void *arg), void *arg);
unsigned int linuxu_dart_held(void);
unsigned int linuxu_dart_release_held(void);

/* Budget getters (test + future metrics). */
uint64_t linuxu_dart_used(void);
uint64_t linuxu_dart_peak(void);
uint64_t linuxu_dart_budget(void);

/* Test/teardown hooks: number of live entries in the token table
 * (must return to 0 on a clean run), and a full flush (clears the
 * table and resets the budget — not used by the driver). */
int linuxu_dart_table_count(void);
void linuxu_dart_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_RT_DART_H */
