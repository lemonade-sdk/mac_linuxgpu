/* linuxu: EDITED (third_party/linux/include/linux/debugfs.h) - CONFIG_DEBUG_FS=y
 * per plan; upstream CONFIG_DEBUG_FS=y branch kept (externs + _Generic
 * macros), CONFIG_DEBUG_FS=n else-branch dropped. linux/fs.h +
 * linux/seq_file.h come from linuxu (other chunk); struct dentry,
 * umode_t, loff_t from linux/types.h + linuxu fs.h. */
// SPDX-License-Identifier: GPL-2.0
/*
 *  debugfs.h - a tiny little debug file system
 *
 *  Copyright (C) 2004 Greg Kroah-Hartman <greg@kroah.com>
 *  Copyright (C) 2004 IBM Inc.
 */

#ifndef _DEBUGFS_H_
#define _DEBUGFS_H_

#include <linux/autoconf.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/kstrtox.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/atomic.h>

struct file;
struct dentry;
struct vfsmount;
struct seq_file;
struct debugfs_blob_wrapper {
	void		*data;
	size_t		size;
};

struct debugfs_reg32 {
	char		*name;
	unsigned long	offset;
};

struct debugfs_regset32 {
	unsigned	int nregs;
	unsigned int	size;
	struct debugfs_reg32	*regs;
	void __iomem		*base;
	char		*prefix;
};

struct debugfs_u32_array {
	u32		*array;
	size_t		count;
};

struct debugfs_short_fops {
	ssize_t (*read)(struct seq_file *s, void *v, loff_t off);
	int	(*open)(struct inode *inode, struct file *file);
	ssize_t (*write)(struct file *file, const char __user *buf, size_t count,
			 loff_t *ppos);
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
};

typedef struct vfsmount *(*debugfs_automount_t)(struct dentry *, void *);

#if defined(CONFIG_DEBUG_FS)

struct dentry *debugfs_lookup(const char *name, struct dentry *parent);

struct dentry *debugfs_create_file_full(const char *name, umode_t mode,
					struct dentry *parent, void *data,
					const void *aux,
					const struct file_operations *fops);
struct dentry *debugfs_create_file_short(const char *name, umode_t mode,
					 struct dentry *parent, void *data,
					 const void *aux,
					 const struct debugfs_short_fops *fops);

#define debugfs_create_file(name, mode, parent, data, fops)			\
	_Generic(fops,							\
		 const struct file_operations *: debugfs_create_file_full,	\
		 const struct debugfs_short_fops *: debugfs_create_file_short,	\
		 struct file_operations *: debugfs_create_file_full,		\
		 struct debugfs_short_fops *: debugfs_create_file_short)	\
		(name, mode, parent, data, NULL, fops)

#define debugfs_create_file_aux(name, mode, parent, data, aux, fops)		\
	_Generic(fops,							\
		 const struct file_operations *: debugfs_create_file_full,	\
		 const struct debugfs_short_fops *: debugfs_create_file_short,	\
		 struct file_operations *: debugfs_create_file_full,		\
		 struct debugfs_short_fops *: debugfs_create_file_short)	\
		(name, mode, parent, data, aux, fops)

struct dentry *debugfs_create_file_unsafe(const char *name, umode_t mode,
				   struct dentry *parent, void *data,
				   const struct file_operations *fops);

void debugfs_create_file_size(const char *name, umode_t mode,
			      struct dentry *parent, void *data,
			      const struct file_operations *fops,
			      loff_t file_size);

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);

struct dentry *debugfs_create_symlink(const char *name, struct dentry *parent,
				      const char *dest);

struct dentry *debugfs_create_automount(const char *name,
					struct dentry *parent,
					debugfs_automount_t f,
					void *data);

void debugfs_remove(struct dentry *dentry);
#define debugfs_remove_recursive debugfs_remove

void debugfs_lookup_and_remove(const char *name, struct dentry *parent);

void *debugfs_get_aux(const struct file *file);

int debugfs_file_get(struct dentry *dentry);
void debugfs_file_put(struct dentry *dentry);

ssize_t debugfs_attr_read(struct file *file, char __user *buf,
			size_t len, loff_t *ppos);
ssize_t debugfs_attr_write(struct file *file, const char __user *buf,
			size_t len, loff_t *ppos);
ssize_t debugfs_attr_write_signed(struct file *file, const char __user *buf,
			size_t len, loff_t *ppos);

int debugfs_change_name(struct dentry *dentry, const char *fmt, ...) __printf(2, 3);

/* ---- simple_attr / DEFINE_DEBUGFS_ATTRIBUTE support (shim) ---- */
struct simple_attribute;
typedef unsigned long long simple_attr_get_t;
typedef long simple_attr_set_t;
static inline void __simple_attr_check_format(const char *fmt, unsigned long long v) { (void)fmt; (void)v; }
static inline int simple_attr_open(struct inode *inode, struct file *file,
				    int (*get)(void *data, u64 *v), int (*set)(void *data, u64 v),
				    const char *fmt)
{	(void)inode; (void)file; (void)get; (void)set; (void)fmt; return 0; }
static inline int simple_attr_release(struct inode *inode, struct file *file) { (void)inode; (void)file; return 0; }
static inline void i_size_write(struct inode *inode, loff_t size) { (void)inode; (void)size; }
static inline loff_t i_size_read(const struct inode *inode) { (void)inode; return 0; }

#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) \
static int __fops ## _open(struct inode *inode, struct file *file) \
{ \
	__simple_attr_check_format(__fmt, 0ull); \
	return simple_attr_open(inode, file, __get, __set, __fmt); \
} \
static const struct file_operations __fops = { \
	.owner   = THIS_MODULE, \
	.open    = __fops ## _open, \
	.release = simple_attr_release, \
	.read    = debugfs_attr_read, \
	.write   = debugfs_attr_write, \
}
#define DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(__fops, __get, __set, __fmt) \
static int __fops ## _open(struct inode *inode, struct file *file) \
{ \
	__simple_attr_check_format(__fmt, 0ull); \
	return simple_attr_open(inode, file, __get, __set, __fmt); \
} \
static const struct file_operations __fops = { \
	.owner   = THIS_MODULE, \
	.open    = __fops ## _open, \
	.release = simple_attr_release, \
	.read    = debugfs_attr_read, \
	.write   = debugfs_attr_write_signed, \
}

void debugfs_create_u8(const char *name, umode_t mode, struct dentry *parent,
		       u8 *value);
void debugfs_create_u16(const char *name, umode_t mode, struct dentry *parent,
			u16 *value);
void debugfs_create_u32(const char *name, umode_t mode, struct dentry *parent,
			u32 *value);
void debugfs_create_u64(const char *name, umode_t mode, struct dentry *parent,
			u64 *value);
void debugfs_create_ulong(const char *name, umode_t mode, struct dentry *parent,
			  unsigned long *value);
void debugfs_create_x8(const char *name, umode_t mode, struct dentry *parent,
		       u8 *value);
void debugfs_create_x16(const char *name, umode_t mode, struct dentry *parent,
			u16 *value);
void debugfs_create_x32(const char *name, umode_t mode, struct dentry *parent,
			u32 *value);
void debugfs_create_x64(const char *name, umode_t mode, struct dentry *parent,
			u64 *value);
void debugfs_create_size_t(const char *name, umode_t mode,
			   struct dentry *parent, size_t *value);
void debugfs_create_atomic_t(const char *name, umode_t mode,
			     struct dentry *parent, atomic_t *value);
void debugfs_create_bool(const char *name, umode_t mode, struct dentry *parent,
			 bool *value);
void debugfs_create_str(const char *name, umode_t mode,
			struct dentry *parent, char **value);

struct dentry *debugfs_create_blob(const char *name, umode_t mode,
				  struct dentry *parent,
				  struct debugfs_blob_wrapper *blob);

void debugfs_create_regset32(const char *name, umode_t mode,
			     struct dentry *parent,
			     struct debugfs_regset32 *regset);

void debugfs_print_regs32(struct seq_file *s, const struct debugfs_reg32 *regs,
			  int nregs, void __iomem *base, char *prefix);

void debugfs_create_u32_array(const char *name, umode_t mode,
			      struct dentry *parent,
			      struct debugfs_u32_array *array);

void debugfs_create_devm_seqfile(struct device *dev, const char *name,
				 struct dentry *parent,
				 int (*read_fn)(struct seq_file *s, void *data));

bool debugfs_initialized(void);

ssize_t debugfs_read_file_bool(struct file *file, char __user *user_buf,
			       size_t count, loff_t *ppos);

ssize_t debugfs_write_file_bool(struct file *file, const char __user *user_buf,
				size_t count, loff_t *ppos);

ssize_t debugfs_read_file_str(struct file *file, char __user *user_buf,
			       size_t count, loff_t *ppos);

#endif /* CONFIG_DEBUG_FS */

#endif /* _DEBUGFS_H_ */
