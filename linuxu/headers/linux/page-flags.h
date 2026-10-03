/* linuxu: SHIM (third_party/linux/include/linux/page-flags.h)
 *
 * The 2026 page-flag accessors (PageDirty/SetPageDirty/...) are generated from
 * page-flags.h. In the userspace dext the struct page carries plain fields and
 * the flags are no-ops; the handful of accessors ttm touches are provided as
 * no-op inlines here. Anything not listed resolves to a no-op via the
 * generated-style macros below.
 */
#ifndef _LINUX_PAGE_FLAGS_H
#define _LINUX_PAGE_FLAGS_H

#include <linux/mm.h>
#include <linux/atomic.h>

#ifndef __PAGEFLAG
/* Default no-op accessors: any Page<X>(page) -> false, Set/Clear -> no-op */
#define __PAGEFLAG(name, test, set, clear)						\
static inline bool Page##name(const struct page *page) { (void)page; return false; } \
static inline void SetPage##name(struct page *page) { (void)page; }		\
static inline void ClearPage##name(struct page *page) { (void)page; }
#endif



__PAGEFLAG(Anon, NULL, NULL, NULL)

__PAGEFLAG(SwapCache, NULL, NULL, NULL)



__PAGEFLAG(Slab, NULL, NULL, NULL)
__PAGEFLAG(ZoneDevice, NULL, NULL, NULL)
__PAGEFLAG(MemPolicy, NULL, NULL, NULL)
__PAGEFLAG(MTLock, NULL, NULL, NULL)
__PAGEFLAG(KSM, NULL, NULL, NULL)
__PAGEFLAG(Active, NULL, NULL, NULL)

__PAGEFLAG(Uncached, NULL, NULL, NULL)
__PAGEFLAG(Mapped, NULL, NULL, NULL)
__PAGEFLAG(UnderlyingIO, NULL, NULL, NULL)

#define LINUXU_PAGE_FLAG(name, bit) \
static inline bool Page##name(const struct page *page) { return !!(__atomic_load_n(&page->flags, __ATOMIC_RELAXED) & (1UL << (bit))); } \
static inline void SetPage##name(struct page *page) { __atomic_fetch_or(&page->flags, 1UL << (bit), __ATOMIC_RELAXED); } \
static inline void ClearPage##name(struct page *page) { __atomic_fetch_and(&page->flags, ~(1UL << (bit)), __ATOMIC_RELAXED); }
LINUXU_PAGE_FLAG(Dirty, 8)
LINUXU_PAGE_FLAG(Locked, 9)
LINUXU_PAGE_FLAG(Referenced, 10)
LINUXU_PAGE_FLAG(SwapBacked, 11)
static inline bool PageHead(const struct page *page) { return page->_compound; }
static inline bool PageTail(const struct page *page) { return page->compound_head != NULL; }
static inline bool PageCompound(const struct page *page) { return PageHead(page) || PageTail(page); }
static inline bool TestClearPageDirty(struct page *page)
{ return !!(__atomic_fetch_and(&page->flags, ~(1UL << 8), __ATOMIC_RELAXED) & (1UL << 8)); }
static inline bool TestSetPageDirty(struct page *page)
{ return !!(__atomic_fetch_or(&page->flags, 1UL << 8, __ATOMIC_RELAXED) & (1UL << 8)); }
static inline bool try_to_release_page(struct page *page, gfp_t gfp) { (void)page; (void)gfp; return true; }

#endif /* _LINUX_PAGE_FLAGS_H */
