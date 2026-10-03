/* linuxu: SHIM (third_party/linux/include/drm/ttm/ttm_backup.h)
 *
 * ttm_backup is the swap-to-file path for BO pages. In userspace we don't
 * have shmem-backed swap files; the handle<->page-pointer conversion helpers
 * are pure and kept verbatim so ttm_pool.c compiles unchanged. The file
 * lifecycle functions (create/backup/copy/drop/fini/bytes_avail) are declared
 * and defined as ENOSYS/inert stubs in linuxu/src/drm/ttm_backup.c — the
 * spike never exercises actual swap-out, so these only need to link.
 */
#ifndef _TTM_BACKUP_H_
#define _TTM_BACKUP_H_

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/fs.h>

struct file;

static inline struct page *
ttm_backup_handle_to_page_ptr(unsigned long handle)
{
	return (struct page *)(handle << 1 | 1);
}

static inline bool ttm_backup_page_ptr_is_handle(const struct page *page)
{
	return (unsigned long)page & 1;
}

static inline unsigned long
ttm_backup_page_ptr_to_handle(const struct page *page)
{
	WARN_ON(!ttm_backup_page_ptr_is_handle(page));
	return (unsigned long)page >> 1;
}

void ttm_backup_drop(struct file *backup, pgoff_t handle);

int ttm_backup_copy_page(struct file *backup, struct page *dst,
			 pgoff_t handle, bool intr, gfp_t additional_gfp);

s64
ttm_backup_backup_page(struct file *backup, struct page *page,
		       bool writeback, pgoff_t idx, gfp_t page_gfp,
		       gfp_t alloc_gfp);

void ttm_backup_fini(struct file *backup);

u64 ttm_backup_bytes_avail(void);

struct file *ttm_backup_shmem_create(loff_t size);

#endif
