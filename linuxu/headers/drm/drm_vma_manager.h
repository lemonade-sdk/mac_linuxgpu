/* linuxu: AS-IS (third_party/linux/include/drm/drm_vma_manager.h) */
#ifndef __DRM_VMA_MANAGER_H__
#define __DRM_VMA_MANAGER_H__

/*
 * Copyright (c) 2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <drm/drm_mm.h>
#include <linux/mm.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/rwlock.h>
#include <linux/types.h>
#include <linux/errno.h>

/* We make up offsets for buffer objects so we can recognize them at
 * mmap time. pgoff in mmap is an unsigned long, so we need to make sure
 * the faked up offset will fit
 */
#if BITS_PER_LONG == 64
#define DRM_FILE_PAGE_OFFSET_START ((0xFFFFFFFFUL >> PAGE_SHIFT) + 1)
#define DRM_FILE_PAGE_OFFSET_SIZE ((0xFFFFFFFFUL >> PAGE_SHIFT) * 256)
#else
#define DRM_FILE_PAGE_OFFSET_START ((0xFFFFFFFUL >> PAGE_SHIFT) + 1)
#define DRM_FILE_PAGE_OFFSET_SIZE ((0xFFFFFFFUL >> PAGE_SHIFT) * 16)
#endif

struct drm_file;

struct drm_vma_offset_file {
	struct rb_node vm_rb;
	struct drm_file *vm_tag;
	unsigned long vm_count;
};

struct drm_vma_offset_node {
	rwlock_t vm_lock;
	struct drm_mm_node vm_node;
	struct rb_root vm_files;
	void *driver_private;
};

struct drm_vma_offset_manager {
	rwlock_t vm_lock;
	struct drm_mm vm_addr_space_mm;
};

void drm_vma_offset_manager_init(struct drm_vma_offset_manager *mgr,
				 unsigned long page_offset, unsigned long size);
void drm_vma_offset_manager_destroy(struct drm_vma_offset_manager *mgr);

struct drm_vma_offset_node *drm_vma_offset_lookup_locked(struct drm_vma_offset_manager *mgr,
							   unsigned long start,
							   unsigned long pages);
int drm_vma_offset_add(struct drm_vma_offset_manager *mgr,
		       struct drm_vma_offset_node *node, unsigned long pages);
void drm_vma_offset_remove(struct drm_vma_offset_manager *mgr,
			   struct drm_vma_offset_node *node);

int drm_vma_node_allow(struct drm_vma_offset_node *node, struct drm_file *tag);
int drm_vma_node_allow_once(struct drm_vma_offset_node *node, struct drm_file *tag);
void drm_vma_node_revoke(struct drm_vma_offset_node *node,
			 struct drm_file *tag);
bool drm_vma_node_is_allowed(struct drm_vma_offset_node *node,
			     struct drm_file *tag);

/**
 * drm_vma_offset_exact_lookup_locked() - Look up node by exact address
 */
static inline struct drm_vma_offset_node *
drm_vma_offset_exact_lookup_locked(struct drm_vma_offset_manager *mgr,
				   unsigned long start,
				   unsigned long pages)
{
	struct drm_vma_offset_node *node;

	node = drm_vma_offset_lookup_locked(mgr, start, pages);
	return (node && node->vm_node.start == start) ? node : NULL;
}

/**
 * drm_vma_offset_lock_lookup() - Lock lookup for extended private use
 */
static inline void drm_vma_offset_lock_lookup(struct drm_vma_offset_manager *mgr)
{
	read_lock(&mgr->vm_lock);
}

/**
 * drm_vma_offset_unlock_lookup() - Release lookup-lock
 */
static inline void drm_vma_offset_unlock_lookup(struct drm_vma_offset_manager *mgr)
{
	read_unlock(&mgr->vm_lock);
}

/**
 * drm_vma_node_reset() - Initialize or reset node object
 */
static inline void drm_vma_node_reset(struct drm_vma_offset_node *node)
{
	memset(node, 0, sizeof(*node));
	node->vm_files.rb_node = NULL;
	rwlock_init(&node->vm_lock);
}

/**
 * drm_vma_node_start() - Return start address for page-based addressing
 */
static inline unsigned long drm_vma_node_start(const struct drm_vma_offset_node *node)
{
	return node->vm_node.start;
}

/**
 * drm_vma_node_size() - Return size (page-based)
 */
static inline unsigned long drm_vma_node_size(const struct drm_vma_offset_node *node)
{
	return node->vm_node.size;
}

/**
 * drm_vma_node_offset_addr() - Return sanitized offset for user-space mmaps
 */
static inline __u64 drm_vma_node_offset_addr(const struct drm_vma_offset_node *node)
{
	return ((__u64)node->vm_node.start) << PAGE_SHIFT;
}

/**
 * drm_vma_node_unmap() - Unmap offset node
 */
static inline void drm_vma_node_unmap(struct drm_vma_offset_node *node,
				      struct address_space *file_mapping)
{
	if (drm_mm_node_allocated(&node->vm_node))
		unmap_mapping_range(file_mapping,
				    drm_vma_node_offset_addr(node),
				    drm_vma_node_size(node) << PAGE_SHIFT, 1);
}

/**
 * drm_vma_node_verify_access() - Access verification helper for TTM
 */
static inline int drm_vma_node_verify_access(struct drm_vma_offset_node *node,
					     struct drm_file *tag)
{
	return drm_vma_node_is_allowed(node, tag) ? 0 : -EACCES;
}

#endif /* __DRM_VMA_MANAGER_H__ */
