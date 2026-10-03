/* Descriptors of a linuxu process as a Linux runtime uses them
 * (linuxu/src/amdgpu-rt/process_file.c). The caller is inside the process
 * (linuxu_process_enter). */
#ifndef LINUXU_RT_PROCESS_FILE_H
#define LINUXU_RT_PROCESS_FILE_H

#include <linux/types.h>

/* open(2) of character device @dev into the current process's descriptor
 * table: the descriptor, or a negative errno. */
int rt_process_open_chrdev(dev_t dev, int flags);

/* ioctl(2) on @fd with its argument block, @bytes at @buf, placed in a user
 * VMA at @va that exists for this call only: the file's unlocked_ioctl
 * copies it in and out with copy_{from,to}_user, and pointers inside the
 * block may address the rest of it from @va. The block is copied back to
 * @buf afterwards. Returns the ioctl's result. */
long rt_process_ioctl(int fd, unsigned int cmd, unsigned long va, void *buf, size_t bytes);

#endif
