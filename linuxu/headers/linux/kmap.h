/* linuxu: SHIM (third_party/linux/include/linux/kmap.h) */
#ifndef __LINUX_KMAP_H
#define __LINUX_KMAP_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/kernel.h>

/* shim: no highmem. struct page carries its mapping via a back-pointer.
 * page_address() is declared here (and re-declared identically in pagemap.h);
 * keep kmap.h self-contained so it works regardless of include order. */

/* userspace shim: kmap is a passthrough (struct page holds the mapping) */
static inline void *kmap(const struct page *page)
{
	return page_address(page);
}
static inline void kunmap(const struct page *page)
{
}
static inline void *kmap_local_page(struct page *page)
{
	return page_address(page);
}
static inline void kunmap_local(const void *kvaddr)
{
}
static inline void *kmap_atomic(const struct page *page)
{
	return page_address(page);
}
static inline void kunmap_atomic(void *kv)
{
}
static inline void *kmap_high_page(const struct page *page)
{
	return page_address(page);
}
static inline void kunmap_high(struct page *page)
{
}

extern void *kmap_local_folio(struct folio *folio, size_t off, size_t size);
extern void *kmap_local_pmd(struct page *page);
extern void kunmap_local_pmd(const void *kvaddr);
extern void *kmap_cont_page(struct page *page);
extern void kunmap_cont_page(struct page *page);
extern void *kmap_cont_local(struct page *page, size_t size);
extern void kunmap_cont_local(const void *kvaddr);

#endif /* __LINUX_KMAP_H */
