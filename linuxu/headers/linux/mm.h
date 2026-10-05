/* linuxu: SHIM (third_party/linux/include/linux/mm.h)
 *
 * SHIM for the KMD's memory-management surface. Provides the small
 * number of types + function declarations that the amdgpu/ttm tree
 * needs: struct page (with the fields the driver actually touches),
 * struct address_space, struct mm_struct, struct vm_area_struct,
 * struct vm_fault, struct vm_operations_struct, pgprot_t, the
 * kmap/kfree page family, and the page-alloc/free entry points.
 *
 * Field notes on struct page (documented for linuxu/src):
 *   .mapping  -- REAL: amdgpu_gart.c:140 writes it; ttm uses it
 *   .private  -- REAL: ttm_pool.c stores order / dma_addr_t here
 *   .lru      -- REAL: ttm_pool.c uses it in list_lru
 *   .index    -- REAL: address_space bookkeeping
 *   .flags    -- REAL: page state bits (shim-owned)
 *   .refcount -- REAL: get_page()/put_page()
 *   .zone_device_data -- REAL: kfd_migrate.c uses it
 *   .pgmap    -- REAL: zone_device
 *   .compound_head -- DUMMY: always 0 (no compound pages in shim)
 *   .compound_dtor -- DUMMY
 *   .dma_addr -- DUMMY: shim stores dma_addr in .private instead
 */
#ifndef _LINUX_MM_H
#define _LINUX_MM_H

#define PAGE_SHIFT	14
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/errno.h>

struct file;
/* The dext cannot choose a host process's virtual address for a DRM GEM
 * mmap. Leave this unsupported until an explicit UserClient mapping API
 * exists; the ordinary in-process buffer path does not call it. */
static inline unsigned long mm_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	(void)file; (void)addr; (void)len; (void)pgoff; (void)flags;
	return (unsigned long)-ENOSYS;
}

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif

#ifndef DIV_ROUND_DOWN
#define DIV_ROUND_DOWN(n, d)	((n) / (d))
#endif
#include <linux/math64.h>
#ifndef MAX
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#endif

#ifndef loff_t
/* loff_t is the global typedef in linux/types.h */
#endif

struct inode;
#include <linux/fs.h>   /* linuxu: full struct inode (i_mapping) */
#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sysinfo.h>
#include <linux/shrinker.h>

/* ---- forward decls ---- */
struct page;
struct address_space;
struct mm_struct;
struct vm_area_struct;
struct file;
struct kmem_cache;

/* ---- pgprot_t (shim: simple tag) ---- */
typedef unsigned long pgprot_t;

/* linuxu tags the cache attribute a driver asks for, so a recorded PFN
 * mapping (remap_pfn_range) can be exported with the same attribute. */
#define LINUXU_PGPROT_NONCACHED		(1UL << 62)
#define LINUXU_PGPROT_WRITECOMBINE	(1UL << 61)
static inline pgprot_t pgprot_noncached(pgprot_t prot)
{ return (prot & ~LINUXU_PGPROT_WRITECOMBINE) | LINUXU_PGPROT_NONCACHED; }
static inline pgprot_t pgprot_decrypted(pgprot_t prot) { return prot; }
static inline pgprot_t pgprot_writecombine(pgprot_t prot)
{ return (prot & ~LINUXU_PGPROT_NONCACHED) | LINUXU_PGPROT_WRITECOMBINE; }
static inline pgprot_t vm_get_page_prot(unsigned long flags) { (void)flags; return 0; }
static inline pgprot_t pgprot_readonly(pgprot_t prot)   { return prot; }
static inline pgprot_t pgprot_nondev(pgprot_t prot)     { return prot; }
static inline pgprot_t pgprot_writecombine_cache(pgprot_t prot) { return prot; }
static inline bool pgprot_none(pgprot_t prot) { return 0; }
static inline pgprot_t __pgprot(unsigned long v) { return (pgprot_t)v; }
static inline unsigned long pgprot_val(pgprot_t prot) { return (unsigned long)prot; }
static inline pgprot_t pgprot_page(pgprot_t prot, unsigned int flags) { (void)flags; return prot; }
/* shim: PAGE_* prot tags are no-op constants; prot manipulation is identity */
#define PAGE_KERNEL		((pgprot_t)0UL)
#define PAGE_READONLY		((pgprot_t)0UL)
#define PAGE_SHARED		((pgprot_t)0UL)
#define PAGE_PRIVATE		((pgprot_t)0UL)
#define PAGE_WRITECOMBINE	((pgprot_t)0UL)
#define PAGE_PFNMAP		((pgprot_t)0UL)
#define PAGE_NOCACHE		((pgprot_t)0UL)
#define __pgprot(v)		((pgprot_t)(v))

/* ---- VM flags from the pinned Linux mm.h ---- */
#define VM_NONE        0x00000000UL
#define VM_READ        0x00000001UL
#define VM_WRITE       0x00000002UL
#define VM_EXEC        0x00000004UL
#define VM_SHARED      0x00000008UL
#define VM_MAYREAD     0x00000010UL
#define VM_MAYWRITE    0x00000020UL
#define VM_MAYEXEC     0x00000040UL
#define VM_MAYSHARE    0x00000080UL
#define VM_GROWSDOWN   0x00000100UL
#define VM_PFNMAP      0x00000400UL
#define VM_LOCKED      0x00002000UL
#define VM_IO          0x00004000UL
#define VM_DONTCOPY    0x00020000UL
#define VM_DONTEXPAND  0x00040000UL
#define VM_ACCOUNT     0x00100000UL
#define VM_NORESERVE   0x00200000UL
#define VM_HUGETLB     0x00400000UL
#define VM_DONTDUMP    0x04000000UL
#define VM_MIXEDMAP    0x10000000UL
/* VM_GROWSUP is only meaningful on parisc, not the ARM64 target. */
#define VM_GROWSUP     VM_NONE
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)

/* ---- fault flags ---- */
#define FAULT_FLAG_WRITE            1
#define FAULT_FLAG_REMOTE           2
#define FAULT_FLAG_INSTRUCTION      4
#define FAULT_FLAG_INTERRUPTIBLE    8
#define FAULT_FLAG_VMA_LOCK         16
#define FAULT_FLAG_ALLOW_RETRY      32
#define FAULT_FLAG_KILLABLE         64
#define FAULT_FLAG_TRIED            128
#define FAULT_FLAG_UNMAPPED         256
#define FAULT_FLAG_UFFD_RETRY       512
#define FAULT_FLAG_RETRY_NOWAIT     1024

static inline bool fault_flag_allow_retry_first(unsigned int flags)
{
	return !!(flags & FAULT_FLAG_ALLOW_RETRY);
}
static inline bool fault_flag_allow_retry_last(unsigned int flags)
{
	return !!(flags & FAULT_FLAG_TRIED);
}
static inline bool fault_flag_need_unlock_irq(unsigned int flags)
{
	return 0;
}
static inline bool fault_flag_need_unlock_vma_lock(unsigned int flags)
{
	return 0;
}
static inline bool fault_flag_need_tlb_flush(unsigned int flags)
{
	return 0;
}
static inline bool fault_flag_allow_userfault(unsigned int flags)
{
	return 0;
}

typedef unsigned int vm_fault_t;
#define VM_FAULT_ERROR         (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_HWPOISON)
#define VM_FAULT_MAJOR         0x0002
#define VM_FAULT_MINOR         0x0004
#define VM_FAULT_NOPAGE        0x0010
#define VM_FAULT_LOCKED        0x0020
#define VM_FAULT_RETRY         0x4000
#define VM_FAULT_OOM           0x0400
#define VM_FAULT_SIGBUS        0x0800
#define VM_FAULT_HWPOISON      0x10000

/* ---- struct page ---- */
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

/* page_address: single owner (mm.h); pagemap.h/kmap.h reuse it. */
extern void *page_address(const struct page *page);

static inline struct page *compound_head(struct page *page)
{
	return page && page->compound_head ? page->compound_head : page;
}
static inline unsigned long page_ref_count(const struct page *page)
{
	return atomic_read(&page->refcount);
}
extern bool linuxu_get_page(struct page *page);
extern void linuxu_put_page(struct page *page);
extern struct page *linuxu_alloc_cpu_page(gfp_t gfp);
static inline void page_ref_inc(struct page *page) { (void)linuxu_get_page(page); }
static inline void page_ref_dec(struct page *page) { linuxu_put_page(page); }
static inline int get_page_unless_zero(struct page *page) { return linuxu_get_page(page); }
static inline unsigned long get_page(struct page *page) { return linuxu_get_page(page); }
static inline void put_page(struct page *page) { linuxu_put_page(page); }

extern void linuxu_lock_page(struct page *page);
extern void linuxu_unlock_page(struct page *page);
extern bool linuxu_trylock_page(struct page *page);
static inline void lock_page(struct page *page) { linuxu_lock_page(page); }
static inline void unlock_page(struct page *page) { linuxu_unlock_page(page); }
static inline int lock_page_killable(struct page *page) { linuxu_lock_page(page); return 0; }
static inline bool trylock_page(struct page *page) { return linuxu_trylock_page(page); }

extern struct page *alloc_pages(gfp_t gfp, unsigned int order);

/* linuxu test hook: grow the page pool/arena (must run before the
 * first alloc_pages); see linuxu/src/mm/page.c. */
extern int linuxu_page_pool_extend(unsigned long pages);
/* Page descriptors held by allocations (until their backing is released). */
extern unsigned long linuxu_page_descriptors(void);
static inline struct page *alloc_page(gfp_t gfp) { return alloc_pages(gfp, 0); }
static inline struct page *alloc_page_vma(gfp_t gfp, struct vm_area_struct *vma, unsigned long arg)
{
	(void)vma;
	return alloc_page(gfp);
}
static inline pgoff_t page_index(const struct page *page)
{
	return page->index;
}
static inline unsigned long page_private(const struct page *page)
{
	return page->private;
}
static inline void set_page_private(struct page *page, unsigned long v)
{
	page->private = v;
}
static inline void clear_page_dirty(struct page *page) { }
static inline int set_page_dirty(struct page *page) { return !(__atomic_fetch_or(&page->flags, 1UL << 8, __ATOMIC_RELAXED) & (1UL << 8)); }
static inline void mark_page_accessed(struct page *page) { __atomic_fetch_or(&page->flags, 1UL << 10, __ATOMIC_RELAXED); }
static inline void copy_highpage(struct page *dst, struct page *src) { memcpy(page_address(dst), page_address(src), PAGE_SIZE); }
static inline void clear_highpage(struct page *page) { if (page) memset(page_address(page), 0, PAGE_SIZE); }
static inline void copy_user_highpage(struct page *dst, struct page *src,
				      unsigned long vaddr, struct vm_area_struct *vma)
{ (void)vaddr; (void)vma; copy_highpage(dst, src); }
static inline void *kmap_local_page_try_from_panic(const struct page *page) { return page ? page_address(page) : NULL; }

/* ---- page <-> pfn ---- */
extern struct page *pfn_to_page(unsigned long pfn);
extern unsigned long page_to_pfn(const struct page *page);
extern bool pfn_valid(unsigned long pfn);
extern int page_to_nid(const struct page *page);
/* page_pgmap is declared in linux/memremap.h (single owner). */

#include <linux/kmap.h>

/* ---- page alloc/free (runtime: linuxu/src/mm/pagealloc.c) ---- */
#ifndef _LINUXU_PAGE_ALLOC_DECLS
#define _LINUXU_PAGE_ALLOC_DECLS
/* 16 KB granularity (PAGE_SHIFT=14): one slot per 16 KB
 * arena page.  W12: backing is a host anon arena; .mapping/.private/
 * .flags writes are real; page_address() returns the host VA. */
extern struct page *alloc_pages(gfp_t gfp, unsigned int order);
extern struct page *alloc_page(gfp_t gfp);
extern struct page *alloc_node_gfp(int nid, gfp_t gfp);
extern unsigned long __get_free_page(unsigned int gfp_mask);
extern void __free_pages(struct page *page, unsigned int order);
extern void __free_pages_ok(struct page *page, unsigned int order);
extern void free_pages(unsigned long address, unsigned int order);
extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
#endif
/* order-0 / node variants (shim: node is ignored, single host node) */
static inline struct page *alloc_pages_node(int nid, gfp_t gfp, unsigned int order) { (void)nid; return alloc_pages(gfp, order); }
static inline void __free_page(struct page *page) { if (page) __free_pages(page, 0); }
static inline void *kvcalloc(size_t n, size_t size, gfp_t gfp) { return kcalloc(n, size, gfp); }
extern void kvfree(const void *addr);
extern void *kvmalloc(size_t size, gfp_t flags);
extern struct page *vmalloc_to_page(const void *addr);
static inline void *kmap_local_page_prot(const struct page *page, pgprot_t prot) { (void)prot; return page ? page_address(page) : NULL; }
static inline void *kmap_local_page_prot_try(const struct page *page, pgprot_t prot) { (void)prot; return page ? page_address(page) : NULL; }
static inline bool PageHighMem(const struct page *page) { (void)page; return false; }
/* PFN_UP / PFN_DOWN (shim: PAGE_SIZE derives from PAGE_SHIFT = 14,
 * 16 KB pages — the whole KMD arithmetic is built on it) */
#define PFN_UP(x)	(((x) + (PAGE_SIZE - 1)) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define PFN_ALIGN(x)	(((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
static inline void clear_page(void *page) { memset(page, 0, PAGE_SIZE); }
static inline void zero_page(struct page *page) { if (page) memset(page_address(page), 0, PAGE_SIZE); }
extern void split_page(struct page *page, unsigned int order);
extern bool is_vmalloc_addr(const void *addr);

/* ---- folio (shim: a folio is a single page in the dext) ---- */
struct folio {
	struct page page;
};
static inline struct folio *page_folio(struct page *p) { return (struct folio *)compound_head(p); }
static inline struct page *folio_page(const struct folio *f) { return (struct page *)&f->page; }
static inline struct page *folio_file_page(struct folio *f, pgoff_t idx)
{ return &f->page + (idx - f->page.index); }
static inline pgoff_t folio_idx(const struct folio *f) { return f->page.index; }
static inline unsigned int folio_order(const struct folio *f) { return f->page._compound ? f->page._pad1 : 0; }
static inline unsigned long folio_nr_pages(const struct folio *f) { return 1UL << folio_order(f); }
extern unsigned long page_to_pfn(const struct page *page);
static inline unsigned long folio_pfn(const struct folio *f) { return page_to_pfn(&f->page); }
extern gfp_t mapping_gfp_mask(struct address_space *mapping);
static inline gfp_t mapping_gfp_constraint(const void *mapping, gfp_t extra)
{ return mapping_gfp_mask((struct address_space *)mapping) & extra; }
static inline void folio_put(struct folio *f) { put_page(&f->page); }
static inline void folio_lock(struct folio *f) { lock_page(&f->page); }
static inline void folio_unlock(struct folio *f) { unlock_page(&f->page); }
static inline void folio_mark_accessed(struct folio *f) { mark_page_accessed(&f->page); }
static inline void folio_mark_dirty(struct folio *f) { set_page_dirty(&f->page); }
static inline bool folio_clear_dirty_for_io(struct folio *f)
{ return !!(__atomic_fetch_and(&f->page.flags, ~(1UL << 8), __ATOMIC_RELAXED) & (1UL << 8)); }
static inline bool folio_test_writeback(struct folio *f)
{ return !!(__atomic_load_n(&f->page.flags, __ATOMIC_RELAXED) & (1UL << 12)); }
static inline void folio_set_reclaim(struct folio *f) { __atomic_fetch_or(&f->page.flags, 1UL << 13, __ATOMIC_RELAXED); }
static inline void folio_clear_reclaim(struct folio *f) { __atomic_fetch_and(&f->page.flags, ~(1UL << 13), __ATOMIC_RELAXED); }
static inline bool folio_mapped(const struct folio *f) { (void)f; return false; }
static inline bool PageReserved(const struct page *page) { (void)page; return false; }
static inline unsigned int PagePrivate(const struct page *page) { (void)page; return 0; }

static inline void *kmap_atomics(struct page *page)
{
	return page_address(page);
}
static inline void kunmap_atomics(struct page *page) { }

/* ---- struct address_space ---- */
struct super_block;
struct dev_pagemap;

struct address_space {
	struct inode *host;
	atomic_long_t i_size;
	unsigned long nrpages;
	void *i_private;
	gfp_t gfp_mask;
};

static inline struct inode *file_inode(const struct file *file)
{
	return file ? file->f_inode : NULL;
}

/* ---- struct mm_struct ----
 * One per linuxu process (linuxu/src/mm/mm.c). mm_users counts tasks and
 * temporary users (mmget/mmput); the last mmput runs the exit_mmap
 * equivalent. mm_count pins the structure itself (mmgrab/mmdrop).
 * VMAs live in mm_rb (keyed by vm_start, never overlapping) and in the
 * vm_start-ordered list headed by mmap. Lookups follow Linux: hold
 * mmap_lock. The uaccess backend takes linuxu_vma_lock instead, so a
 * copy_{from,to}_user under a held mmap_lock cannot self-deadlock;
 * linuxu_mm_insert_vma/linuxu_mm_remove_vma take both. */
struct mmu_notifier_subscriptions;
struct mm_struct {
	unsigned long mmap_base;
	unsigned long task_size;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, arg_start, arg_end, env_start, env_end;
	unsigned long free_area;
	struct vm_area_struct *mmap;
	struct rb_root mm_rb;
	int map_count;
	atomic_t mm_users;
	atomic_t mm_count;
	struct rw_semaphore mmap_lock;
	struct rw_semaphore linuxu_vma_lock;
	struct mmu_notifier_subscriptions *notifier_subscriptions;
	struct task_struct *owner;
	unsigned long flags;
	unsigned long *pgd;
};

/* mm->flags bits (linuxu): mmu notifiers have been released for exit. */
#define MMF_LINUXU_RELEASED	0

/* ---- struct vm_area_struct ----
 * Field set verified against every vma deref in the driver (W15 audit):
 * amdgpu_gem.c/ttm_bo_vm.c use vm_start, vm_end, vm_pgoff, vm_mm,
 * vm_ops, vm_flags, vm_page_prot, vm_private_data, vm_file.  All are
 * present; the retry protocol (FAULT_FLAG_ALLOW_RETRY/TRIED) is
 * neutralized in-process — the fault path never re-faults, so the
 * struct only needs to compile and carry the data the test reads. */
typedef unsigned long vm_flags_t;
#define EMPTY_VMA_FLAGS	((vm_flags_t)0UL)

struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	pgoff_t vm_pgoff;
	struct vm_area_struct *vm_next;
	struct vm_area_struct *vm_prev;
	struct mm_struct *vm_mm;
	const struct vm_operations_struct *vm_ops;
	unsigned long vm_flags;
	void *vm_private_data;
	pgprot_t vm_page_prot;
	unsigned long vm_nonlinear_flags;
	struct file *vm_file;
	/* linuxu: registry node in vm_mm->mm_rb (linuxu_mm_insert_vma). */
	struct rb_node vm_rb;
	/* linuxu: kernel-side backing used by the uaccess backend. When
	 * linuxu_kmap is set it returns the kernel address for @addr and
	 * stores in *len how many bytes are contiguous from there (it must
	 * not sleep); otherwise linuxu_kaddr maps vm_start linearly. A VMA
	 * with neither (a bare reservation) faults like an unpopulated
	 * PROT_NONE mapping. */
	void *linuxu_kaddr;
	void *(*linuxu_kmap)(struct vm_area_struct *vma, unsigned long addr,
			     unsigned long *len);
	void *linuxu_backing;
	/* linuxu: what remap_pfn_range/io_remap_pfn_range installed: the
	 * first PFN mapped at vm_start, the bytes mapped from vm_start and the
	 * page protection. There are no CPU page tables to populate; the
	 * owner of the mm exports the recorded range (for a PCI BAR, the bus
	 * address range) to the real address space instead. */
	unsigned long linuxu_pfn;
	unsigned long linuxu_pfn_bytes;
	pgprot_t linuxu_pfn_prot;
};

/* vma-based vm_flags_clear (vendor 2026) */
static inline void vm_flags_clear(struct vm_area_struct *vma, vm_flags_t clear_mask)
{
	vma->vm_flags &= ~clear_mask;
}
static inline vm_flags_t vm_flags_test_and_clear(struct vm_area_struct *vma, vm_flags_t mask)
{
	vm_flags_t ret = vma->vm_flags & mask;
	vma->vm_flags &= ~mask;
	return ret;
}


/* ---- struct vm_fault (defined before vm_operations_struct so the
 * function-pointer fields see the complete type) ---- */
struct vm_fault {
	struct {
		struct vm_area_struct *vma;
		gfp_t gfp_mask;
		pgoff_t pgoff;
		unsigned long address;
		unsigned long real_address;
	};
	unsigned long flags;
	struct page *page;
	struct page *cow_page;
};

/* ---- struct vm_operations_struct ---- */
struct vm_operations_struct {
	void (*open)(struct vm_area_struct *vma);
	void (*close)(struct vm_area_struct *vma);
	vm_fault_t (*fault)(struct vm_fault *vmf);
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
	int (*access)(struct vm_area_struct *vma, unsigned long addr,
		      void *buf, int len, int write);
	const char *(*name)(struct vm_area_struct *vma);
	int (*split)(struct vm_area_struct *vma, unsigned long addr);
	void (*unmap)(struct vm_area_struct *vma,
		      unsigned long start, unsigned long end);
};

/* ---- vma helpers (runtime: linuxu/src/mm/mm.c) ---- */
extern struct vm_area_struct *vm_area_alloc(struct mm_struct *mm);
extern void vm_area_free(struct vm_area_struct *vma);
/* Upstream static inline (linux/mm.h); a malformed range has no pages. */
static inline unsigned long vma_pages(const struct vm_area_struct *vma)
{
	return vma && vma->vm_end >= vma->vm_start ?
		(vma->vm_end - vma->vm_start) >> PAGE_SHIFT : 0;
}
extern int vma_munmap(struct vm_area_struct *vma,
		      unsigned long start, unsigned long end);

/* No host process page-table insertion backend exists. A successful Linux
 * fault result would claim that an absent mapping is ready for use. */
typedef struct { unsigned long pfn; } pfn_t;
static inline vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t prot)
{	(void)vma; (void)addr; (void)pfn; (void)prot; return VM_FAULT_SIGBUS; }
static inline vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{	(void)vma; (void)addr; (void)pfn; return VM_FAULT_SIGBUS; }
static inline vm_fault_t vmf_insert_page(struct vm_area_struct *vma, unsigned long addr, struct page *page)
{	(void)vma; (void)addr; (void)page; return VM_FAULT_SIGBUS; }
static inline vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, bool write)
{	(void)vma; (void)addr; (void)pfn; (void)write; return VM_FAULT_SIGBUS; }
static inline void pfn_t_init(pfn_t *t, unsigned long pfn, unsigned long flags) { (void)t; (void)pfn; (void)flags; }
static inline void pfn_t_clear(pfn_t *t) { (void)t; }
static inline unsigned long pfn_t_to_pfn(pfn_t t) { (void)t; return 0; }
static inline bool pfn_t_has_prot(pfn_t t) { (void)t; return false; }
static inline pgprot_t pfn_t_pgprot(pfn_t t) { (void)t; return PAGE_KERNEL; }
static inline bool is_cow_mapping(unsigned long vm_flags) { (void)vm_flags; return false; }
static inline void vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags) { vma->vm_flags |= flags; }
static inline bool vma_is_cow_mapping(const struct vm_area_struct *vma) { (void)vma; return false; }



/* ---- shmem / file helpers (used by ttm_tt.c) ---- */
extern struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
						pgoff_t index, gfp_t gfp);
extern void shmem_truncate_range(struct inode *inode,
				 loff_t lstart, loff_t lend);
static inline struct folio *shmem_read_folio_gfp(struct address_space *mapping,
						  pgoff_t idx, gfp_t gfp)
{
	return (struct folio *)shmem_read_mapping_page_gfp(mapping, idx, gfp);
}

extern void mmput(struct mm_struct *mm);

/* vendor 2026 get_order (asm-generic/getorder.h) */
static inline int get_order(unsigned long size)
{
	/* Linux defines the zero-size result as word bits minus PAGE_SHIFT. */
	if (!size) return BITS_PER_LONG - PAGE_SHIFT;
	return fls_long((size - 1) >> PAGE_SHIFT);
}

/* __pa (upstream asm-generic/page.h): userspace identity map */
static inline phys_addr_t __pa(const void *x)
{
	return (phys_addr_t)(unsigned long)x;
}

static inline void *__va(phys_addr_t x)
{
	return (void *)(unsigned long)x;
}

/* vendor 2026 mm.h page helpers (amdgpu_gart.c / gmc_v9_0.c) */
extern void *page_to_virt(struct page *page);
extern int page_is_ram(unsigned long pfn);
/* num_possible_nodes: see linux/mmzone.h */
extern void mmput_async(struct mm_struct *mm);
/* ---- mm lifetime, VMA registry and mmap_lock (linuxu/src/mm/mm.c) ---- */
extern struct mm_struct *get_task_mm(struct task_struct *task);
extern struct mm_struct *mmget(struct mm_struct *mm);
extern bool mmget_not_zero(struct mm_struct *mm);
extern void mmgrab(struct mm_struct *mm);
extern void mmdrop(struct mm_struct *mm);
extern struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr);
extern struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);
extern struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
		unsigned long start_addr, unsigned long end_addr);
extern bool mmap_read_trylock(struct mm_struct *mm);
extern int mmap_read_lock_killable(struct mm_struct *mm);
extern int mmap_write_lock_killable(struct mm_struct *mm);
extern void mmap_assert_locked(const struct mm_struct *mm);
extern void mmap_assert_write_locked(const struct mm_struct *mm);

/* A new mm with mm_users == mm_count == 1 and no VMAs. */
extern struct mm_struct *linuxu_mm_alloc(void);
/* Insert @vma ([vm_start, vm_end), page aligned) into @mm. The caller holds
 * mmap_write_lock(mm). Returns -EINVAL for a malformed range and -EEXIST
 * when it overlaps an existing VMA. On success the mm owns @vma: the last
 * mmput calls vm_ops->close, drops vm_file and frees it. */
extern int linuxu_mm_insert_vma(struct mm_struct *mm, struct vm_area_struct *vma);
/* Unlink @vma; the caller holds mmap_write_lock(mm) and owns @vma again. */
extern void linuxu_mm_remove_vma(struct mm_struct *mm, struct vm_area_struct *vma);
/* exit_mmap's notifier step: ->release on every mmu_notifier subscription
 * and MMU_NOTIFY_RELEASE on every interval subscription. Idempotent. */
extern void linuxu_mm_exit(struct mm_struct *mm);
/* find_vma for callers holding mmap_lock or linuxu_vma_lock. */
extern struct vm_area_struct *linuxu_find_vma_locked(struct mm_struct *mm,
						     unsigned long addr);
/* Replace task->mm (and active_mm) under the get_task_mm lock; returns the
 * old mm. Reference counts are the caller's business, as in exit_mm. */
extern struct mm_struct *linuxu_task_set_mm(struct task_struct *task,
					    struct mm_struct *mm);

/* ---- vma mapping helpers (kfd_svm.c) ---- */
static inline bool vma_is_initial_heap(const struct vm_area_struct *vma)
{
	(void)vma;
	return false;
}

static inline bool vma_is_initial_stack(const struct vm_area_struct *vma)
{
	(void)vma;
	return false;
}

/* ---- mmap locking downgrade (upstream mmap_lock.h) ---- */
extern void mmap_write_downgrade(struct mm_struct *mm);

/* ---- vm_mmap (upstream linux/mm.h) — shim: returns a fixed fake VA ---- */
extern unsigned long vm_mmap(struct file *file, unsigned long addr,
			      unsigned long len, unsigned long prot,
			      unsigned long flags, unsigned long offset);

/* ---- remap_pfn_range (kfd_doorbell.c, kfd_chardev.c; upstream mm/memory.c)
 * Records the range in the VMA (linuxu_pfn*) and sets the flags Linux sets
 * (VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP). The range must lie in
 * the VMA and continue whatever an earlier call on the same VMA recorded.
 * Runtime: linuxu/src/mm/mm.c. */
extern int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
			   unsigned long pfn, unsigned long size, pgprot_t prot);

static inline int io_remap_pfn_range(struct vm_area_struct *vma,
				      unsigned long addr,
				      unsigned long pfn, unsigned long size,
				      pgprot_t prot)
{
	return remap_pfn_range(vma, addr, pfn, size, prot);
}

/* linuxu: CONFIG_INIT_ON_FREE_DEFAULT_ON=n, so always false. */
static inline bool want_init_on_free(void)
{
	return false;
}

extern void unmap_mapping_range(struct address_space *mapping,
			 unsigned long start, unsigned long end, int even_munmap);


/* vma_flags_t (vendor 2026) — bitmask for VMA flags */
typedef struct {
	unsigned long bits[2];
} vma_flags_t;

#define VMA_NORESERVE_BIT	21
#define EMPTY_VMA_FLAGS		((vma_flags_t){ })

static inline vma_flags_t linuxu_mk_vma_flags(size_t count,
					    const unsigned int *bits)
{
	vma_flags_t flags = EMPTY_VMA_FLAGS;
	for (size_t i = 0; i < count; ++i) {
		unsigned int bit = bits[i];
		if (bit < sizeof(flags.bits) * 8)
			flags.bits[bit / BITS_PER_LONG] |= 1UL << (bit % BITS_PER_LONG);
	}
	return flags;
}
#define mk_vma_flags(...) linuxu_mk_vma_flags( \
	sizeof((const unsigned int[]){__VA_ARGS__}) / sizeof(unsigned int), \
	(const unsigned int[]){__VA_ARGS__})

static inline vm_flags_t vma_flags_to_legacy(vma_flags_t flags)
{
	return flags.bits[0];
}

#ifndef PAGE_ALIGN
#define PAGE_ALIGN(addr) ALIGN((addr), PAGE_SIZE)
#endif

static inline int check_move_unevictable_folios(void *folio)
{
	(void)folio;
	return 0;
}

struct folio_batch {
	unsigned int nr;
	void *folios[16];
};
static inline void __folio_batch_release(void *batch)
{
	struct folio_batch *folios = batch;
	for (unsigned int i = 0; i < folios->nr; i++) folio_put(folios->folios[i]);
	folios->nr = 0;
}

static inline void mapping_set_unevictable(void *mapping) { (void)mapping; }
static inline void mapping_clear_unevictable(void *mapping) { (void)mapping; }
#endif /* _LINUX_MM_H */
