/* Internal character-device number registry; no host /dev node is created. */
#ifndef LINUXU_RT_CHRDEV_H
#define LINUXU_RT_CHRDEV_H

#include <linux/types.h>

struct file_operations;

#ifdef __cplusplus
extern "C" {
#endif

/* A returned fops pointer is owned by the registrant and remains valid only
 * until that registration is removed. Callers must coordinate teardown. */
int linuxu_chrdev_lookup(dev_t dev,
			const struct file_operations **fops);
/* The major of the character device registered as @name with file
 * operations (what /proc/devices lists). -ENXIO when none is registered. */
int linuxu_chrdev_find(const char *name, unsigned int *major);

struct file;
/* Open @dev as Linux chrdev_open does: a new open file description on a
 * character-device inode (i_rdev = @dev) whose f_op is the registered fops,
 * then ->open, which may replace f_op (DRM's stub does). On success *out
 * holds one reference; fput releases it through f_op->release. The caller
 * installs it in a descriptor table (get_unused_fd_flags + fd_install).
 * Runtime: linuxu/src/shims/fd.c. */
int linuxu_chrdev_open(dev_t dev, int flags, struct file **out);

#ifdef __cplusplus
}
#endif

#endif
