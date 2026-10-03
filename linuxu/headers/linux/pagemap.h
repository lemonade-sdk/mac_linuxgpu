/* linuxu: SHIM (third_party/linux/include/linux/pagemap.h) */
#ifndef __LINUX_PAGEMAP_H
#define __LINUX_PAGEMAP_H

#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/compiler.h>
#include <linux/gfp.h>
#include <linux/bitops.h>

/* page<->pfn (shim: struct page has a back-pointer, see mm.h contract) */
extern struct page *pfn_to_page(unsigned long pfn);
extern unsigned long page_to_pfn(const struct page *page);
extern unsigned long __page_to_pfn(const struct page *page);
extern unsigned long __pfn_to_phys(unsigned long pfn);
extern phys_addr_t pfn_to_phys(unsigned long pfn);
extern unsigned long virt_to_phys(unsigned long vaddr);
extern unsigned long phys_to_virt(phys_addr_t addr);

#define PFN_UP(x)		(((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x)		((x) >> PAGE_SHIFT)
#define PHYS_PFN(x)		((x) >> PAGE_SHIFT)
#define PFN_PHYS(pfn)		((pfn) << PAGE_SHIFT)
#define PAGE_ALIGN(addr)	ALIGN((addr), PAGE_SIZE)
#define PAGE_MASK		(~(PAGE_SIZE - 1))
#define virt_to_page(p)		virt_to_page_internal((p))
extern struct page *virt_to_page_internal(const void *p);

static inline int pfn_to_nid(unsigned long pfn)
{
	(void)pfn;
	return 0;
}
#endif /* __LINUX_PAGEMAP_H */

/* linuxu: upstream linux/pagemap.h */
#ifndef offset_in_page
#define offset_in_page(p)	((unsigned long)(p) & (PAGE_SIZE - 1))
#endif
