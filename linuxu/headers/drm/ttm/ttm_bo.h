/* linuxu: AS-IS (third_party/linux/include/drm/ttm/ttm_bo.h) — struct
 * ttm_buffer_object carried verbatim (upstream field order preserved;
 * amdgpu_bo embeds it).  <drm/drm_gem.h> and "ttm_device.h" resolve to
 * their linuxu shadows. */
#ifndef _TTM_BO_API_H_
#define _TTM_BO_API_H_

#include <drm/drm_gem.h>
#include <drm/dma_resv.h>

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/cleanup.h>
#include <linux/workqueue.h>

#include "ttm_device.h"

/* Default number of pre-faulted pages in the TTM fault handler */
#define TTM_BO_VM_NUM_PREFAULT 16

struct iosys_map;

struct ttm_global;
struct ttm_device;
struct ttm_placement;
struct ttm_place;
struct ttm_resource;
struct ttm_resource_manager;
struct ttm_tt;

/**
 * enum ttm_bo_type
 */
enum ttm_bo_type {
	ttm_bo_type_device,
	ttm_bo_type_kernel,
	ttm_bo_type_sg
};

/**
 * struct ttm_buffer_object
 *
 * Field order matches upstream so that struct amdgpu_bo (which embeds
 * this as its first member) stays layout-compatible.
 */
struct ttm_buffer_object {
	struct drm_gem_object base;

	/*
	 * Members constant at init.
	 */
	struct ttm_device *bdev;
	enum ttm_bo_type type;
	uint32_t page_alignment;
	void (*destroy) (struct ttm_buffer_object *);

	/*
	* Members not needing protection.
	*/
	struct kref kref;

	/*
	 * Members protected by the bo::resv::reserved lock.
	 */
	struct ttm_resource *resource;
	struct ttm_tt *ttm;
	bool deleted;
	struct ttm_lru_bulk_move *bulk_move;
	unsigned priority;
	unsigned pin_count;

	/**
	 * @delayed_delete: Work item used when we can't delete the BO
	 * immediately
	 */
	struct work_struct delayed_delete;

	/**
	 * @sg: external source of pages and DMA addresses, protected by the
	 * reservation lock.
	 */
	struct sg_table *sg;
};

#define TTM_BO_MAP_IOMEM_MASK 0x80

/**
 * struct ttm_bo_kmap_obj
 *
 * Object describing a kernel mapping.
 */
struct ttm_bo_kmap_obj {
	void *virtual;
	struct page *page;
	enum {
		ttm_bo_map_iomap        = 1 | TTM_BO_MAP_IOMEM_MASK,
		ttm_bo_map_vmap         = 2,
		ttm_bo_map_kmap         = 3,
		ttm_bo_map_premapped    = 4 | TTM_BO_MAP_IOMEM_MASK,
	} bo_kmap_type;
	struct ttm_buffer_object *bo;
};

/**
 * struct ttm_operation_ctx
 *
 * Context for TTM operations like changing buffer placement or general
 * memory allocation.
 */
struct ttm_operation_ctx {
	/** @interruptible: Sleep interruptible if sleeping. */
	bool interruptible;
	/** @no_wait_gpu: Return immediately if the GPU is busy. */
	bool no_wait_gpu;
	/** @gfp_retry_mayfail: Use __GFP_RETRY_MAYFAIL | __GFP_NOWARN. */
	bool gfp_retry_mayfail;
	/** @allow_res_evict: Allow eviction of reserved BOs. */
	bool allow_res_evict;
	/** @resv: Reservation object to be used together with @allow_res_evict. */
	struct dma_resv *resv;
	/** @bytes_moved: Statistics on how many bytes have been moved. */
	uint64_t bytes_moved;
};

struct ttm_lru_walk;

/** struct ttm_lru_walk_ops - Operations for a LRU walk. */
struct ttm_lru_walk_ops {
	/** @process_bo: Process this bo. */
	s64 (*process_bo)(struct ttm_lru_walk *walk, struct ttm_buffer_object *bo);
};

/**
 * struct ttm_lru_walk_arg - Common part for the variants of BO LRU walk.
 */
struct ttm_lru_walk_arg {
	/** @ctx: Pointer to the struct ttm_operation_ctx. */
	struct ttm_operation_ctx *ctx;
	/** @ticket: The struct ww_acquire_ctx if any. */
	struct ww_acquire_ctx *ticket;
	/** @trylock_only: Only use trylock for locking. */
	bool trylock_only;
};

/**
 * struct ttm_lru_walk - Structure describing a LRU walk.
 */
struct ttm_lru_walk {
	/** @ops: Pointer to the ops structure. */
	const struct ttm_lru_walk_ops *ops;
	/** @arg: Common bo LRU walk arguments. */
	struct ttm_lru_walk_arg arg;
};

s64 ttm_lru_walk_for_evict(struct ttm_lru_walk *walk, struct ttm_device *bdev,
			   struct ttm_resource_manager *man, s64 target);

/**
 * struct ttm_bo_shrink_flags - flags to govern the bo shrinking behaviour
 */
struct ttm_bo_shrink_flags {
	u32 purge : 1;
	u32 writeback : 1;
	u32 allow_move : 1;
};

long ttm_bo_shrink(struct ttm_operation_ctx *ctx, struct ttm_buffer_object *bo,
		   const struct ttm_bo_shrink_flags flags);

bool ttm_bo_shrink_suitable(struct ttm_buffer_object *bo, struct ttm_operation_ctx *ctx);

bool ttm_bo_shrink_avoid_wait(void);

/**
 * ttm_bo_reserve:
 *
 * Locks a buffer object for validation.
 */
static inline int ttm_bo_reserve(struct ttm_buffer_object *bo,
				 bool interruptible, bool no_wait,
				 struct ww_acquire_ctx *ticket)
{
	int ret = 0;

	if (no_wait) {
		bool success;

		if (WARN_ON(ticket))
			return -EBUSY;

		success = dma_resv_trylock(bo->base.resv);
		return success ? 0 : -EBUSY;
	}

	if (interruptible)
		ret = dma_resv_lock_interruptible(bo->base.resv, ticket);
	else
		ret = dma_resv_lock(bo->base.resv, ticket);
	if (ret == -EINTR)
		return -ERESTARTSYS;
	return ret;
}

/**
 * ttm_bo_reserve_slowpath:
 *
 * This is called after ttm_bo_reserve returns -EAGAIN.
 */
static inline int ttm_bo_reserve_slowpath(struct ttm_buffer_object *bo,
					  bool interruptible,
					  struct ww_acquire_ctx *ticket)
{
	if (interruptible) {
		int ret = dma_resv_lock_slow_interruptible(bo->base.resv,
							   ticket);
		if (ret == -EINTR)
			ret = -ERESTARTSYS;
		return ret;
	}
	dma_resv_lock_slow(bo->base.resv, ticket);
	return 0;
}

void ttm_bo_move_to_lru_tail(struct ttm_buffer_object *bo);

static inline void
ttm_bo_move_to_lru_tail_unlocked(struct ttm_buffer_object *bo)
{
	spin_lock(&bo->bdev->lru_lock);
	ttm_bo_move_to_lru_tail(bo);
	spin_unlock(&bo->bdev->lru_lock);
}

static inline void ttm_bo_assign_mem(struct ttm_buffer_object *bo,
				     struct ttm_resource *new_mem)
{
	WARN_ON(bo->resource);
	bo->resource = new_mem;
}

/**
 * ttm_bo_move_null - assign memory for a buffer object.
 */
static inline void ttm_bo_move_null(struct ttm_buffer_object *bo,
				    struct ttm_resource *new_mem)
{
	ttm_resource_free(bo, &bo->resource);
	ttm_bo_assign_mem(bo, new_mem);
}

/**
 * ttm_bo_unreserve
 *
 * Unreserve a previous reservation of @bo.
 */
static inline void ttm_bo_unreserve(struct ttm_buffer_object *bo)
{
	ttm_bo_move_to_lru_tail_unlocked(bo);
	dma_resv_unlock(bo->base.resv);
}

/**
 * ttm_kmap_obj_virtual
 *
 * Returns the virtual address of a buffer object area mapped by ttm_bo_kmap.
 */
static inline void *ttm_kmap_obj_virtual(struct ttm_bo_kmap_obj *map,
					 bool *is_iomem)
{
	*is_iomem = !!(map->bo_kmap_type & TTM_BO_MAP_IOMEM_MASK);
	return map->virtual;
}

int ttm_bo_wait_ctx(struct ttm_buffer_object *bo,
		    struct ttm_operation_ctx *ctx);
int ttm_bo_validate(struct ttm_buffer_object *bo,
		    struct ttm_placement *placement,
		    struct ttm_operation_ctx *ctx);
void ttm_bo_fini(struct ttm_buffer_object *bo);
void ttm_bo_set_bulk_move(struct ttm_buffer_object *bo,
			  struct ttm_lru_bulk_move *bulk);
bool ttm_bo_eviction_valuable(struct ttm_buffer_object *bo,
			      const struct ttm_place *place);
int ttm_bo_init_reserved(struct ttm_device *bdev, struct ttm_buffer_object *bo,
			 enum ttm_bo_type type, struct ttm_placement *placement,
			 uint32_t alignment, struct ttm_operation_ctx *ctx,
			 struct sg_table *sg, struct dma_resv *resv,
			 void (*destroy)(struct ttm_buffer_object *));
int ttm_bo_init_validate(struct ttm_device *bdev, struct ttm_buffer_object *bo,
			 enum ttm_bo_type type, struct ttm_placement *placement,
			 uint32_t alignment, bool interruptible,
			 struct sg_table *sg, struct dma_resv *resv,
			 void (*destroy)(struct ttm_buffer_object *));
int ttm_bo_kmap(struct ttm_buffer_object *bo, unsigned long start_page,
		unsigned long num_pages, struct ttm_bo_kmap_obj *map);
void ttm_bo_kunmap(struct ttm_bo_kmap_obj *map);
void *ttm_bo_kmap_try_from_panic(struct ttm_buffer_object *bo, unsigned long page);
int ttm_bo_vmap(struct ttm_buffer_object *bo, struct iosys_map *map);
void ttm_bo_vunmap(struct ttm_buffer_object *bo, struct iosys_map *map);
int ttm_bo_mmap_obj(struct vm_area_struct *vma, struct ttm_buffer_object *bo);
s64 ttm_bo_swapout(struct ttm_device *bdev, struct ttm_operation_ctx *ctx,
		   struct ttm_resource_manager *man, gfp_t gfp_flags,
		   s64 target);
void ttm_bo_pin(struct ttm_buffer_object *bo);
void ttm_bo_unpin(struct ttm_buffer_object *bo);
int ttm_bo_evict_first(struct ttm_device *bdev,
		       struct ttm_resource_manager *man,
		       struct ttm_operation_ctx *ctx);
int ttm_bo_access(struct ttm_buffer_object *bo, unsigned long offset,
		  void *buf, int len, int write);
vm_fault_t ttm_bo_vm_reserve(struct ttm_buffer_object *bo,
			     struct vm_fault *vmf);
vm_fault_t ttm_bo_vm_fault_reserved(struct vm_fault *vmf,
				    pgprot_t prot,
				    pgoff_t num_prefault);
vm_fault_t ttm_bo_vm_fault(struct vm_fault *vmf);
void ttm_bo_vm_open(struct vm_area_struct *vma);
void ttm_bo_vm_close(struct vm_area_struct *vma);
int ttm_bo_vm_access(struct vm_area_struct *vma, unsigned long addr,
		     void *buf, int len, int write);
vm_fault_t ttm_bo_vm_dummy_page(struct vm_fault *vmf, pgprot_t prot);

int ttm_bo_mem_space(struct ttm_buffer_object *bo,
		     struct ttm_placement *placement,
		     struct ttm_resource **mem,
		     struct ttm_operation_ctx *ctx);

void ttm_bo_unmap_virtual(struct ttm_buffer_object *bo);
/*
 * ttm_bo_util.c
 */
int ttm_mem_io_reserve(struct ttm_device *bdev,
		       struct ttm_resource *mem);
void ttm_mem_io_free(struct ttm_device *bdev,
		     struct ttm_resource *mem);
void ttm_move_memcpy(bool clear, u32 num_pages,
		     struct ttm_kmap_iter *dst_iter,
		     struct ttm_kmap_iter *src_iter);
int ttm_bo_move_memcpy(struct ttm_buffer_object *bo,
		       struct ttm_operation_ctx *ctx,
		       struct ttm_resource *new_mem);
int ttm_bo_move_accel_cleanup(struct ttm_buffer_object *bo,
			      struct dma_fence *fence, bool evict,
			      bool pipeline,
			      struct ttm_resource *new_mem);
void ttm_bo_move_sync_cleanup(struct ttm_buffer_object *bo,
			      struct ttm_resource *new_mem);
int ttm_bo_pipeline_gutting(struct ttm_buffer_object *bo);
pgprot_t ttm_io_prot(struct ttm_buffer_object *bo, struct ttm_resource *res,
		     pgprot_t tmp);
void ttm_bo_tt_destroy(struct ttm_buffer_object *bo);
int ttm_bo_populate(struct ttm_buffer_object *bo,
		    struct ttm_operation_ctx *ctx);
int ttm_bo_setup_export(struct ttm_buffer_object *bo,
			struct ttm_operation_ctx *ctx);

/* Driver LRU walk helpers initially targeted for shrinking. */

/**
 * struct ttm_bo_lru_cursor - Iterator cursor for TTM LRU list looping
 */
struct ttm_bo_lru_cursor {
	/** @res_curs: Embedded struct ttm_resource_cursor. */
	struct ttm_resource_cursor res_curs;
	/** @bo: Buffer object pointer if a buffer object is refcounted. */
	struct ttm_buffer_object *bo;
	/** @needs_unlock: Valid iff @bo != NULL. */
	bool needs_unlock;
	/** @arg: Pointer to common BO LRU walk arguments. */
	struct ttm_lru_walk_arg *arg;
};

void ttm_bo_lru_cursor_fini(struct ttm_bo_lru_cursor *curs);

struct ttm_bo_lru_cursor *
ttm_bo_lru_cursor_init(struct ttm_bo_lru_cursor *curs,
		       struct ttm_resource_manager *man,
		       struct ttm_lru_walk_arg *arg);

struct ttm_buffer_object *ttm_bo_lru_cursor_first(struct ttm_bo_lru_cursor *curs);

struct ttm_buffer_object *ttm_bo_lru_cursor_next(struct ttm_bo_lru_cursor *curs);

/*
 * Defines needed to use autocleanup (linux/cleanup.h) with struct ttm_bo_lru_cursor.
 */
DEFINE_CLASS(ttm_bo_lru_cursor, struct ttm_bo_lru_cursor *,
	     if (_T) {ttm_bo_lru_cursor_fini(_T); },
	     ttm_bo_lru_cursor_init(curs, man, arg),
	     struct ttm_bo_lru_cursor *curs, struct ttm_resource_manager *man,
	     struct ttm_lru_walk_arg *arg);
static inline void *
class_ttm_bo_lru_cursor_lock_ptr(class_ttm_bo_lru_cursor_t *_T)
{ return *_T; }
#define class_ttm_bo_lru_cursor_is_conditional false

/**
 * ttm_bo_lru_for_each_reserved_guarded() - Iterate over buffer objects owning
 * resources on LRU lists.
 * @_cursor: struct ttm_bo_lru_cursor to use for the iteration.
 * @_man: The resource manager whose LRU lists to iterate over.
 * @_arg: The struct ttm_lru_walk_arg to govern the LRU walk.
 * @_bo: The struct ttm_buffer_object pointer pointing to the buffer object
 * for the current iteration.
 *
 * Iterate over all resources of @_man and for each resource, attempt to
 * reference and lock (using the locking mode detailed in @_ctx) the buffer
 * object it points to. If successful, assign @_bo to the address of the
 * buffer object and update @_cursor. The iteration is guarded in the
 * sense that @_cursor will be initialized before looping start and cleaned
 * up at looping termination, even if terminated prematurely by, for
 * example a return or break statement. Exiting the loop will also unlock
 * (if needed) and unreference @_bo.
 *
 * Return: If locking of a bo returns an error, then iteration is terminated
 * and @_bo is set to a corresponding error pointer. It's illegal to
 * dereference @_bo after loop exit.
 */
#define ttm_bo_lru_for_each_reserved_guarded(_cursor, _man, _arg, _bo)	\
	scoped_guard(ttm_bo_lru_cursor, _cursor, _man, _arg)		\
		for ((_bo) = ttm_bo_lru_cursor_first(_cursor);		\
		       !IS_ERR_OR_NULL(_bo);				\
		       (_bo) = ttm_bo_lru_cursor_next(_cursor))

#endif
