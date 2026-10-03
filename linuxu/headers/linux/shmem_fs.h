/* linuxu: SHIM (third_party/linux/include/linux/shmem_fs.h) */
#ifndef __LINUX_SHMEM_FS_H
#define __LINUX_SHMEM_FS_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/fs.h>

int shmem_writeout(struct folio *folio, void *plug, void *data);


/* vendor 2026: vma_flags_t flags */
struct file *shmem_file_setup_with_mnt(struct vfsmount *huge_mnt,
				       const char *name, loff_t size,
				       vma_flags_t flags);
struct file *shmem_file_setup(const char *name, loff_t size,
			      vma_flags_t flags);
#endif
