/* linuxu: SHIM (third_party/linux/include/linux/agp_backend.h)
 *
 * AGP backend surface for ttm_agp_backend.c. The linuxu build has no AGP
 * host, so agp_allocate_memory/agp_free_memory/agp_bind_memory are
 * function-like macros that no-op to the "no AGP" outcome the TTM caller
 * expects (allocation fails with NULL, bind/unbind succeed trivially).
 */
#ifndef _AGP_BACKEND_H
#define _AGP_BACKEND_H 1

#include <linux/list.h>
#include <linux/types.h>

struct page;
struct scatterlist;

enum chipset_type {
	NOT_SUPPORTED,
	SUPPORTED,
};

struct agp_version {
	u16 major;
	u16 minor;
};

struct agp_bridge_data;

/*
 * The agp_memory structure has information about the block of agp memory
 * allocated.  A caller may manipulate the next and prev pointers to link
 * each allocated item into a list.  These pointers are ignored by the
 * backend.
 */
struct agp_memory {
	struct agp_memory *next;
	struct agp_memory *prev;
	struct agp_bridge_data *bridge;
	struct page **pages;
	size_t page_count;
	int key;
	int num_scratch_pages;
	off_t pg_start;
	u32 type;
	u32 physical;
	bool is_bound;
	bool is_flushed;
	/* list of agp_memory mapped to the aperture */
	struct list_head mapped_list;
	/* DMA-mapped addresses */
	struct scatterlist *sg_list;
	int num_sg;
};

#define AGP_NORMAL_MEMORY 0

#define AGP_USER_TYPES (1 << 16)
#define AGP_USER_MEMORY (AGP_USER_TYPES)
#define AGP_USER_CACHED_MEMORY (AGP_USER_TYPES + 1)

/* no AGP host in the shim: allocation always fails, bind trivially ok */
#define agp_allocate_memory(bridge, pages, type)	((struct agp_memory *)NULL)
#define agp_free_memory(mem)				do { (void)(mem); } while (0)
#define agp_bind_memory(mem, pg_start)			0
#define agp_unbind_memory(mem)				0

#endif /* _AGP_BACKEND_H */
