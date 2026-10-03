/* linuxu: SHIM (third_party/linux/include/linux/sync_file.h)
 *
 * The struct is complete (vendor 2026 layout) because the driver
 * dereferences sync_file->file (amdgpu_cs.c fd_install path).
 */
#ifndef __LINUX_SYNC_FILE_H
#define __LINUX_SYNC_FILE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/dma-fence.h>

struct file;

struct sync_file {
	struct file		*file;
	char			user_name[32];
	wait_queue_head_t	wq;
	unsigned long		flags;
	struct dma_fence	*fence;
};

extern struct sync_file *sync_file_create(struct dma_fence *fence);
extern void sync_file_put(struct sync_file *sf);
extern void sync_file_destroy(struct sync_file *sf);

/*
 * sync.c includes sync_file.h BEFORE dma-fence.h; the driver and the
 * rest of the tree always have struct dma_fence complete by the time
 * sync_file_create() is called, so forward-declare it here to keep this
 * header self-contained in that include order.
 */
struct dma_fence;


struct dma_fence *sync_file_get_fence(int fd);

#endif /* __LINUX_SYNC_FILE_H */
