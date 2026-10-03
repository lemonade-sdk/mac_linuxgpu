/* linuxu: SHIM (third_party/linux/include/linux/scatterlist.h) */
#ifndef _LINUX_SCATTERLIST_H
#define _LINUX_SCATTERLIST_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/atomic.h>   /* linuxu: struct page def below needs atomic_t */
#include <linux/list.h>     /* linuxu: struct page def below needs list_head */
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/limits.h>

struct kmem_cache;
struct address_space;

/*
 * struct page: declared (NOT #include'd) so scatterlist.h does not
 * drag <linux/mm.h> into the dma-mapping header (dma-mapping.h
 * includes this file *before* mm.h would otherwise provide struct
 * page — the header pair is mutually referential, and a struct
 * declaration is all scatterlist needs for `struct page *page`).
 * <linux/mm.h> (which #includes scatterlist.h itself) remains the
 * canonical definition provider.
 *
 * linuxu: the full definition is provided here instead of only a
 * declaration.  Reason: several shims (src/drm/ttm.c, W5) size
 * `struct page` locally (kcalloc(n, sizeof(struct page))) in a
 * TU that does not include <linux/mm.h>; a pure forward declaration
 * makes sizeof a compile error.  The definition is the same shim
 * struct mm.h carries (single source: keep the two in sync — the
 * mm.h copy is the canonical one; this copy exists for the
 * sg-from-pages math above and for shims that never see mm.h).
 * Guarded so a TU that includes <linux/mm.h> first (the canonical
 * definition) does not redefine it here.
 */
#ifndef _LINUXU_STRUCT_PAGE_DEFINED
#define _LINUXU_STRUCT_PAGE_DEFINED
struct page {
	union {
		unsigned long flags;
		struct {
			unsigned long slab:1;
			unsigned long _compound:1;
			unsigned long _pad:62;
		};
	};
	union {
		struct {
			struct list_head lru;
			struct page *next;
		};
		unsigned long compound_dtor;
	};
	union {
		unsigned long private;
		void *zone_device_data;
	};
	struct address_space *mapping;
	pgoff_t index;
	atomic_t refcount;
	union {
		struct page *compound_head;
		int _compound_pad;
	};
	unsigned int _pad1;
	unsigned int _pad2;
	union {
		struct {
			struct list_head pcp;
			int pages;
			int zone;
		};
		struct kmem_cache *slab_cache;
		struct address_space *pgmap;
	};
};
#endif /* _LINUXU_STRUCT_PAGE_DEFINED */

/* PAGE_SIZE fallback: the shim's mm model uses 16 KB pages
 * (PAGE_SHIFT=14) — the allocator
 * (linuxu/src/mm/page.c) backs every page slot with 16 KB.  If a TU
 * only includes this header (not <linux/mm.h>) the fallback must
 * agree with the allocator's granularity or sg page-count math is
 * wrong. */
#ifndef PAGE_SIZE
#define PAGE_SIZE	(1UL << 14)
#endif

/*
 * struct scatterlist - scatter-gather list element (upstream layout).
 * Shim: page/offset kept for source compatibility; dma_addr/length are
 * the effective addresses.
 */
struct scatterlist {
	dma_addr_t	dma_address;
	unsigned int	length;
	unsigned int	offset;
	unsigned int	flags;
	struct page	*page;
};

#define SG_DMA_ADDR		BIT(0)
#define SG_LAST			BIT(1)
#define SG_CHAIN		BIT(2)

struct sg_table {
	struct scatterlist	*sgl;
	unsigned int		orig_nents;
	unsigned int		nents;
	dma_addr_t		dma_address;
	unsigned int		dma_length;
};

static inline void sg_init_table(struct scatterlist *sgl, unsigned int n)
{
	unsigned int i;
	for (i = 0; i < n; i++) {
		sgl[i].dma_address = 0;
		sgl[i].length = 0;
		sgl[i].offset = 0;
		sgl[i].flags = 0;
		sgl[i].page = NULL;
	}
	if (n)
		sgl[n - 1].flags |= SG_LAST;
}

static inline void sg_init_one(struct scatterlist *sg, const void *buf,
			       unsigned int buflen)
{
	sg->dma_address = (dma_addr_t)(unsigned long)buf;
	sg->length = buflen;
	sg->offset = 0;
	sg->flags = SG_LAST;
	sg->page = NULL;
}

static inline void sg_set_buf(struct scatterlist *sg, const void *buf,
			      unsigned int buflen)
{
	sg->dma_address = (dma_addr_t)(uintptr_t)buf;
	sg->length = buflen;
	sg->offset = 0;
	sg->page = NULL;
}

static inline void sg_set_page(struct scatterlist *sg, struct page *page,
			       unsigned int buflen, unsigned int offset)
{
	sg->dma_address = 0;
	sg->length = buflen;
	sg->offset = offset;
	sg->page = page;
}

extern void *page_address(const struct page *page);
extern unsigned long page_to_pfn(const struct page *page);
#ifdef LINUXU_DEXT_DK
extern void *linuxu_sg_cpu_address(struct scatterlist *sg);
#endif
static inline void *sg_virt(struct scatterlist *sg)
{
#ifdef LINUXU_DEXT_DK
	return linuxu_sg_cpu_address(sg);
#else
	if (sg->page) {
		void *base = page_address(sg->page);
		return base ? (char *)base + sg->offset : NULL;
	}
	return (void *)(uintptr_t)sg->dma_address;
#endif
}
/* vendor 2026: macro forms so the driver can assign sg_dma_address(sg) = x */
#define sg_dma_address(sg)	((sg)->dma_address)
#define sg_dma_len(sg)		((sg)->length)
static inline unsigned int sg_offset(const struct scatterlist *sg)
{
	return sg->offset;
}
static inline struct page *sg_page(const struct scatterlist *sg)
{
	return sg->page;
}
static inline dma_addr_t sg_phys(const struct scatterlist *sg)
{
	return sg->page ? (dma_addr_t)page_to_pfn(sg->page) * PAGE_SIZE +
		sg->offset : sg->dma_address;
}
static inline bool sg_is_last(const struct scatterlist *sg)
{
	return sg->flags & SG_LAST;
}
static inline bool sg_is_chain(const struct scatterlist *sg)
{
	return sg->flags & SG_CHAIN;
}
static inline void sg_mark_last(struct scatterlist *sg)
{
	sg->flags = (sg->flags | SG_LAST) & ~SG_CHAIN;
}
static inline void sg_mark_end(struct scatterlist *sg)
{
	sg->flags = (sg->flags | SG_LAST) & ~SG_CHAIN;
}
static inline void sg_unmark_end(struct scatterlist *sg)
{
	sg->flags &= ~SG_LAST;
}
static inline void sg_unmark_last(struct scatterlist *sg)
{
	sg->flags &= ~SG_LAST;
}

static inline struct scatterlist *sg_chain_ptr(const struct scatterlist *sg)
{
	return (struct scatterlist *)sg->page;
}

static inline struct scatterlist *sg_next(struct scatterlist *sg)
{
	if (!sg || sg_is_last(sg))
		return NULL;
	sg++;
	return sg_is_chain(sg) ? sg_chain_ptr(sg) : sg;
}

static inline void sg_chain(struct scatterlist *prv, unsigned int prv_nents,
			    struct scatterlist *sgl)
{
	struct scatterlist *link = &prv[prv_nents - 1];
	link->page = (struct page *)sgl;
	link->offset = link->length = 0;
	link->dma_address = 0;
	link->flags = SG_CHAIN;
}

static inline int sg_nents_for_len(struct scatterlist *sgl, u64 buflen)
{
	int count = 0;
	if (!buflen) return 0;
	for (; sgl; sgl = sg_next(sgl)) {
		if (count == INT_MAX) return -EINVAL;
		count++;
		if (buflen <= sgl->length) return count;
		buflen -= sgl->length;
	}
	return -EINVAL;
}

/*
 * sg_table runtime (host shim: identity dma mapping, so the sg page
 * pointer is the dma address; linuxu/src/drm/ttm.c provides the
 * linuxu_dma_ops glue the driver uses).
 */
static inline int sg_alloc_table(struct sg_table *table, unsigned int nents,
				 gfp_t gfp)
{
	if (!table) return -EINVAL;
	*table = (struct sg_table){0};
	if (!nents) return -EINVAL;
	table->sgl = kmalloc_array(nents, sizeof(*table->sgl), gfp);
	if (!table->sgl) return -ENOMEM;
	sg_init_table(table->sgl, nents);
	table->orig_nents = table->nents = nents;
	return 0;
}

/* Keep each shim page in its own segment: adjacent synthetic PFNs do not
 * imply adjacent DriverKit allocations or one DART mapping. */
static inline int sg_alloc_table_from_pages_segment(struct sg_table *table,
		struct page **pages, unsigned int npages, unsigned int offset,
		unsigned long size, unsigned int max_segment, gfp_t gfp)
{
	unsigned long count;
	if (!table) return -EINVAL;
	*table = (struct sg_table){0};
	if (!pages || !npages || !size || offset >= PAGE_SIZE ||
	    max_segment < PAGE_SIZE || size > (unsigned long)npages * PAGE_SIZE - offset)
		return -EINVAL;
	count = size / PAGE_SIZE +
		((size % PAGE_SIZE + offset + PAGE_SIZE - 1) / PAGE_SIZE);
	for (unsigned long i = 0; i < count; i++)
		if (!pages[i]) return -EINVAL;
	int error = sg_alloc_table(table, (unsigned int)count, gfp);
	if (error) return error;
	for (unsigned int i = 0; i < count; i++) {
		unsigned int length = PAGE_SIZE - offset;
		if (length > size) length = size;
		sg_set_page(&table->sgl[i], pages[i], length, offset);
		size -= length;
		offset = 0;
	}
	return 0;
}

static inline int sg_alloc_table_from_pages(struct sg_table *table,
		struct page **pages, unsigned int npages, unsigned int offset,
		size_t size, gfp_t gfp)
{
	return sg_alloc_table_from_pages_segment(table, pages, npages, offset,
						size, UINT_MAX, gfp);
}

static inline void sg_free_table(struct sg_table *table)
{
	kfree(table->sgl);
	table->sgl = NULL;
	table->nents = table->orig_nents = 0;
}

static inline void sg_free_table_chained(struct sg_table *table)
{
	sg_free_table(table);
}
extern int sg_split(struct sg_table *table, struct sg_table *split, unsigned int pos);
extern void sg_free_table_chained(struct sg_table *table);
extern int sg_copy_from_buffer(struct sg_table *table, size_t offset,
			       size_t buflen, const void *buf);
extern int sg_copy_to_buffer(struct sg_table *table, size_t offset,
			     void *buf, size_t buflen);
extern struct scatterlist *sgl_next(struct scatterlist *sgl);
extern int sg_set_buf_continue(struct scatterlist *sg, const void *buf,
			       unsigned int buflen);
extern unsigned int sg_copy_from_buffer_continue(struct sg_table *table,
						 size_t buflen,
						 const void *buf);
extern unsigned int sg_copy_to_buffer_continue(struct sg_table *table,
					       void *buf, size_t buflen);
/* sgtable iteration (vendor 2026 scatterlist.h) */
#define for_each_sg(sglist, sg, nr, i) \
	for ((i) = 0, (sg) = (sglist); (i) < (nr); (i)++, (sg) = sg_next(sg))
#define for_each_sgtable_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

/*
 * sg_add_page (vendor 2026; host shim: one sg entry per page, identity
 * mapping). Used by kfd_mqd_manager and amdgpu_ttm for CPU pages.
 */
static inline int sg_add_page(struct sg_table *table, struct page *page,
			       unsigned int len, unsigned int offset, gfp_t gfp)
{
	struct scatterlist *sg;
	if (table->orig_nents == UINT_MAX) return -EOVERFLOW;
	sg = krealloc_array(table->sgl, table->orig_nents + 1, sizeof(*sg), gfp);
	if (!sg) return -ENOMEM;
	if (table->orig_nents) sg_unmark_end(&sg[table->orig_nents - 1]);
	table->sgl = sg;
	sg_init_table(&sg[table->orig_nents], 1);
	sg_set_page(&sg[table->orig_nents], page, len, offset);
	table->orig_nents++;
	table->nents = table->orig_nents;
	return 0;
}

static inline int sg_add_page_chain(struct sg_table *table, struct page *page,
				     unsigned int len, unsigned int offset,
				     gfp_t gfp)
{
	return sg_add_page(table, page, len, offset, gfp);
}


/* Page iterators include the partial first/last pages of each segment. */
struct sg_page_iter {
	struct scatterlist	*sg;
	unsigned long		sg_pgoffset;
	unsigned int		__nents;
	int			__pg_advance;
};

struct sg_dma_page_iter {
	struct sg_page_iter base;
};

static inline void __sg_page_iter_start(struct sg_page_iter *piter,
					struct scatterlist *sglist,
					unsigned int nents, unsigned long pgoffset)
{
	piter->sg = sglist;
	piter->sg_pgoffset = pgoffset;
	piter->__nents = nents;
	piter->__pg_advance = 0;
}

static inline bool __sg_page_iter_next(struct sg_page_iter *piter)
{
	if (!piter->__nents || !piter->sg) return false;
	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;
	while (piter->__nents && piter->sg) {
		unsigned long pages = ((unsigned long)piter->sg->offset +
			piter->sg->length + PAGE_SIZE - 1) / PAGE_SIZE;
		if (piter->sg_pgoffset < pages) return true;
		piter->sg_pgoffset -= pages;
		piter->sg = sg_next(piter->sg);
		piter->__nents--;
	}
	return false;
}

static inline bool __sg_page_iter_dma_next(struct sg_dma_page_iter *dma_iter)
{
	struct sg_page_iter *piter = &dma_iter->base;
	if (!piter->__nents || !piter->sg) return false;
	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;
	while (piter->__nents && piter->sg) {
		unsigned long pages = ((unsigned long)sg_dma_len(piter->sg) +
			PAGE_SIZE - 1) / PAGE_SIZE;
		if (piter->sg_pgoffset < pages) return true;
		piter->sg_pgoffset -= pages;
		piter->sg = sg_next(piter->sg);
		piter->__nents--;
	}
	return false;
}

static inline struct page *sg_page_iter_page(struct sg_page_iter *piter)
{
	return sg_page(piter->sg) + piter->sg_pgoffset;
}

static inline dma_addr_t sg_page_iter_dma_address(struct sg_dma_page_iter *dma_iter)
{
	return sg_dma_address(dma_iter->base.sg) +
		dma_iter->base.sg_pgoffset * PAGE_SIZE;
}

#define for_each_sgtable_page(sgt, piter, pgoffset)	\
	for (__sg_page_iter_start((piter), (sgt)->sgl, (sgt)->orig_nents, (pgoffset)); \
	     __sg_page_iter_next(piter);)

#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset)	\
	for (__sg_page_iter_start(&(dma_iter)->base, (sgt)->sgl, (sgt)->nents, (pgoffset)); \
	     __sg_page_iter_dma_next(dma_iter);)


#endif /* _LINUX_SCATTERLIST_H */
