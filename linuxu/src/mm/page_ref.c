/* linuxu shim: page_ref — get_page/put_page helpers
 * (MAPPING).  The inline fast paths live in the
 * header over atomic_t; this file hosts the non-inline variants. */
#include <linux/mm.h>
#include <linux/atomic.h>

/* get_page/put_page are header inlines; the page pool owns the
 * refcount.  Nothing extra is required at link time for P0. */

/* __get_free_page / __free_page single-page helpers over the pool.
 * __free_page is a header inline (linux/mm.h) — only __get_free_page
 * needs a runtime definition. */
extern struct page *alloc_pages(gfp_t gfp, unsigned int order);
extern void __free_pages(struct page *page, unsigned int order);

unsigned long __get_free_page(unsigned int gfp_mask)
{
	struct page *p = alloc_pages(gfp_mask, 0);

	/* the page stays allocated (refcount 1); the caller frees it with
	 * __free_page(pfn_to_page(pfn)) — hand back the host VA, NOT the
	 * struct-page pointer (P0 bug: the struct page was never reclaimable
	 * as a VA, so every __get_free_page caller leaked a 16 KB slot). */
	return p ? (unsigned long)page_address(p) : 0;
}
