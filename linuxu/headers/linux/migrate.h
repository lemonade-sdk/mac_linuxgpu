/* linuxu: SHIM (third_party/linux/include/linux/migrate.h) — page-migration
 * surface used by kfd_migrate.c. The single-node userspace model has no
 * real pages to move; the API is provided so the unmodified .c compiles. */
#ifndef _LINUX_MIGRATE_H
#define _LINUX_MIGRATE_H

#include <linux/mm.h>
#include <linux/types.h>

struct migration_target_control {
	int nr_succeeded;
	int nr_failed;
	gfp_t gfp_mask;
	int flags;
};

struct page *alloc_migration_target(struct folio *folio, int page_nid,
				    unsigned long private,
				    struct migration_target_control *ctrl,
				    unsigned long *result);

void migration_put_page(struct page *page);

/* ---- migrate_vma (upstream shape, linux/migrate.h) ---- */
#define MIGRATE_PFN_VALID	(1UL << 0)
#define MIGRATE_PFN_MIGRATE	(1UL << 1)
#define MIGRATE_PFN_WRITE	(1UL << 3)
#define MIGRATE_PFN_COMPOUND	(1UL << 4)
#define MIGRATE_PFN_SHIFT	6

static inline struct page *migrate_vma_get_page(unsigned long mpfn)
{
	if (!(mpfn & MIGRATE_PFN_VALID))
		return NULL;
	return pfn_to_page(mpfn >> MIGRATE_PFN_SHIFT);
}

static inline unsigned long migrate_vma_get_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	return (pfn << MIGRATE_PFN_SHIFT) | MIGRATE_PFN_VALID;
}

/* raw-pfn helpers (upstream linux/migrate.h) */
static inline unsigned long migrate_pfn(unsigned long pfn)
{
	return (pfn << MIGRATE_PFN_SHIFT) | MIGRATE_PFN_VALID;
}

static inline struct page *migrate_pfn_to_page(unsigned long mpfn)
{
	if (!(mpfn & MIGRATE_PFN_VALID))
		return NULL;
	return pfn_to_page(mpfn >> MIGRATE_PFN_SHIFT);
}

enum migrate_vma_direction {
	MIGRATE_VMA_SELECT_SYSTEM = 1 << 0,
	MIGRATE_VMA_SELECT_DEVICE_PRIVATE = 1 << 1,
	MIGRATE_VMA_SELECT_DEVICE_COHERENT = 1 << 2,
	MIGRATE_VMA_SELECT_COMPOUND = 1 << 3,
};

struct migrate_vma {
	struct vm_area_struct	*vma;
	unsigned long		*dst;
	unsigned long		*src;
	unsigned long		cpages;
	unsigned long		npages;
	unsigned long		start;
	unsigned long		end;
	void			*pgmap_owner;
	unsigned long		flags;
	struct page		*fault_page;
};

extern int migrate_vma_setup(struct migrate_vma *args);
extern void migrate_vma_pages(struct migrate_vma *migrate);
extern void migrate_vma_finalize(struct migrate_vma *migrate);

#endif /* _LINUX_MIGRATE_H */
