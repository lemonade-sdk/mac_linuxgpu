/* linuxu: SHIM (third_party/linux/include/linux/vmalloc.h)
 *
 * Virtual memory allocator API surface. All non-trivial functions are
 * declared extern (runtime: linuxu/src/kmem/vmalloc.c). Trivial
 * accessors are static inline as upstream.
 */
#ifndef _LINUX_VMALLOC_H
#define _LINUX_VMALLOC_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/slab.h>

/*
 * struct vm_struct - describes a contiguous vmalloc region.
 */
struct vm_struct {
	unsigned long  start;
	unsigned long  size;
	unsigned long  flags;
	void         (*caller)(struct vm_struct *);
	struct vm_struct *next;
};

/* vm_struct flags are distinct from the VMAs declared in mm.h. */
#define VM_IOREMAP          0x00000001
#define VM_ALLOC            0x00000002
#define VM_MAP              0x00000004
#define VM_USERMAP          0x00000008
#define VM_DMA_COHERENT     0x00000010
#define VM_UNINITIALIZED    0x00000020
#define VM_NO_GUARD         0x00000040
#define VM_KASAN            0x00000080
#define VM_FLUSH_RESET_PERMS 0x00000100
#define VM_MAP_PUT_PAGES    0x00000200
#define VM_ALLOW_HUGE_VMAP  0x00000400
#define VM_DEFER_KMEMLEAK   0
#define VM_SPARSE           0x00001000

#define VMALLOC_TO_PAGE(p)	vmalloc_to_page(p)

/* ---- API (runtime: linuxu/src/kmem/vmalloc.c) ---- */
extern void *vmalloc(unsigned long size);
extern void *vzalloc(unsigned long size);

/* kvmalloc family (vendor 2026; in-process: slab-backed) */
extern void *kvmalloc(size_t size, gfp_t flags);
extern void *kvzalloc(size_t size, gfp_t flags);
extern void *vmalloc_node(unsigned long size, int node);
extern void *vzalloc_node(unsigned long size, int node);
extern void *vmalloc_user(unsigned long size);
extern void *vmalloc_32(unsigned long size);
extern void *vmalloc_32_user(unsigned long size);
extern void *vmalloc_huge(unsigned long size, gfp_t gfp_mask);
extern void *__vmalloc(unsigned long size, gfp_t gfp_mask);
extern void *__vmalloc_node_range(unsigned long size, unsigned long align,
				  unsigned long start, unsigned long end,
				  gfp_t gfp_mask);
extern void *vmalloc_array(size_t n, size_t size);
extern void *__vmalloc_array(size_t n, size_t size, gfp_t flags);
extern void *vcalloc(size_t n, size_t size);
extern void *__vcalloc(size_t n, size_t size, gfp_t flags);
extern void  vfree(const void *addr);
extern void  vfree_atomic(const void *addr);
extern void *vmap(struct page **pages, unsigned int count,
		  unsigned long flags, pgprot_t prot);
extern void  vunmap(const void *addr);
extern void *vm_map_ram(struct page **pages, unsigned int count, int node);
extern void  vm_unmap_ram(const void *mem, unsigned int count);
extern void  vm_unmap_aliases(void);
extern int   remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
				 unsigned long pgoff);
extern int   remap_vmalloc_range_partial(struct vm_area_struct *vma,
					 unsigned long vaddr,
					 void *addr, unsigned long size,
					 unsigned long pgoff);

/* ---- vm_struct helpers ---- */
extern struct vm_struct *get_vm_area(unsigned long size, unsigned long flags);
extern struct vm_struct *remove_vm_area(const void *addr);
extern struct vm_struct *find_vm_area(const void *addr);

static inline size_t get_vm_area_size(const struct vm_struct *area)
{
	return area ? area->size : 0;
}
extern bool is_vmalloc_addr(const void *addr);
static inline bool vmap_to_page(const void *addr, struct page **page)
{
	if (!page) return false;
	*page = vmalloc_to_page(addr);
	return *page != NULL;
}
static inline void set_vm_flush_reset_perms(void *addr) { }

#endif /* _LINUX_VMALLOC_H */
