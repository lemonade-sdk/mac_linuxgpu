/* In-process anonymous files with owned descriptor references. */
#ifndef __LINUX_ANON_INODES_H
#define __LINUX_ANON_INODES_H
#include <linux/fs.h>
#include <linux/file.h>
static inline int anon_inode_getfd(const char *name,
	const struct file_operations *fops, void *priv, int flags)
{
	int fd = get_unused_fd_flags(flags);
	if (fd < 0) return fd;
	struct file *file = anon_inode_getfile(name, fops, priv, flags);
	if (IS_ERR(file)) { put_unused_fd(fd); return PTR_ERR(file); }
	fd_install(fd, file);
	return fd;
}
static inline int anon_inode_getfd_compat(const char *name,
	const struct file_operations *fops, void *priv, int flags)
{ return anon_inode_getfd(name, fops, priv, flags); }
#endif
