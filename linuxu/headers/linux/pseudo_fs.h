/* linuxu: SHIM (third_party/linux/include/linux/pseudo_fs.h) */
#ifndef __LINUX_PSEUDO_FS_H
#define __LINUX_PSEUDO_FS_H

struct pseudo_fs_context {
	const struct super_operations *ops;
	const struct export_operations *eops;
	const struct xattr_handler * const *xattr;
	const struct dentry_operations *dops;
	unsigned long magic;
	unsigned int s_d_flags;
};
struct fs_context;
struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic);
void kill_anon_super(struct super_block *sb);
#endif
