/* linuxu: SHIM (third_party/linux/include/linux/fs.h)
 *
 * VFS surface the driver touches: struct file/inode/dentry, fmode,
 * loff_t, FMODE_*, and the mmap_lock helpers.
 * Runtime: linuxu/src/shims/fs.c
 */
#ifndef __LINUX_FS_H
#define __LINUX_FS_H

#include <linux/kdev_t.h>
#include <linux/chardev.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <linux/fs_context.h>
#include <linux/poll.h>
struct vm_area_struct;   /* linuxu: forward decl for file_operations::mmap */
#include <linux/refcount.h>
#include <linux/list.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/rwsem.h>
#include <linux/module.h>

struct inode;
struct dentry;
typedef void *fl_owner_t;
struct file;
struct vfsmount;
struct path;
struct kiocb;
struct address_space;
struct super_block;
struct block_device;
struct gendisk;
struct file_ra_state;
struct fown_struct {
	int			pid;
	int			fd;
};

/* fmode_t / O_* (Linux values) */
#define FMODE_READ		0x00000001
#define FMODE_WRITE		0x00000002
#define FMODE_LSEEK		0x00000004
#define FMODE_PREAD		0x00000008
#define FMODE_PWRITE		0x00000010
#define FMODE_EXEC		0x00000020
#define FMODE_STREAM		0x00000040
#define FMODE_NOCTTY		0x00000080
#define FMODE_NONOTIFY		0x00000100
#define FMODE_NOWAIT		0x00000800
#define FMODE_NDELAY		0x00001000
#define FMODE_NOATIME		0x00002000
#define FMODE_NOSIGNAL		0x00080000
#define FMODE_PATH		0x00100000

#define O_RDONLY		0
#define O_WRONLY		1
#define O_RDWR			2
#define O_ACCMODE		3
#define O_CREAT			0x40
#define O_EXCL			0x80
#define O_NOCTTY		0x100
#define O_TRUNC			0x200
#define O_APPEND		0x400
#define O_NONBLOCK		0x800
#define O_DSYNC			0x1000
#define O_DIRECT		0x4000
#define O_LARGEFILE		0x8000
#define O_DIRECTORY		0x10000
#define O_NOFOLLOW		0x20000
#define O_NOATIME		0x40000
#define O_CLOEXEC		0x80000
#define O_SYNC			0x101000
#define O_RSYNC			0x1000
#define O_MAND			0x40
#define O_ASYNC			0x2000
#define O_NDELAY		O_NONBLOCK
#define O_PATH			0x200000
#define O_TMPFILE		(O_TMPFILE | O_DIRECTORY)

#define S_IFMT		00170000
#define S_IFSOCK	0140000
#define S_IFLNK		0120000
#define S_IFREG		0100000
#define S_IFBLK		0060000
#define S_IFDIR		0040000
#define S_IFCHR		0020000
#define S_IFIFO		0010000
#define S_ISUID		0004000
#define S_ISGID		0002000
#define S_ISVTX		0001000
#define S_IRWXU		00700
#define S_IRUSR		00400
#define S_IWUSR		00200
#define S_IXUSR		00100
#define S_IRWXG		00070
#define S_IRGRP		00040
#define S_IWGRP		00020
#define S_IXGRP		00010
#define S_IRWXO		00007
#define S_IRWXUGO		(S_IRWXU | S_IRWXG | S_IRWXO)
#define S_IWUGO		(S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO		(S_IXUSR | S_IXGRP | S_IXOTH)
#define S_IROTH		00004
#define S_IWOTH		00002
#define S_IXOTH		00001

typedef u32 kuid_t;
typedef u32 kgid_t;

struct path {
	struct vfsmount	*mnt;
	struct dentry	*dentry;
};

struct file {
	const struct file_operations	*f_op;
	const struct path		f_path;
	struct inode		*f_inode;
	unsigned int		f_flags;
	fmode_t			f_mode;
	loff_t			f_pos;
	struct file_ra_state	*f_ra;
	spinlock_t		f_lock;
	int			f_count;
	unsigned int		f_pos_lock;
	void			*f_priv;
	struct fown_struct	f_owner;
	int			f_flags2;
	struct address_space	*f_mapping;
	void		*private_data;
	bool linuxu_allocated;
};

struct inode {
	umode_t			i_mode;
	unsigned int		i_flags;
	refcount_t		i_count;
	kuid_t			i_uid;
	kgid_t			i_gid;

	const struct inode_operations	*i_op;
	struct super_block	*i_sb;
	struct address_space	*i_mapping;

	u64			i_ino;
	unsigned int		i_nlink;
	dev_t			i_rdev;
	loff_t			i_size;
	time64_t		i_atime_sec;
	time64_t		i_mtime_sec;
	time64_t		i_ctime_sec;
	u32			i_atime_nsec;
	u32			i_mtime_nsec;
	u32			i_ctime_nsec;
	u32			i_generation;
	spinlock_t		i_lock;
	unsigned short		i_bytes;
	u8			i_blkbits;
	blkcnt_t		i_blocks;

	struct rw_semaphore	i_rwsem;
	unsigned long		dirtied_when;
	unsigned long		dirtied_time_when;
	struct hlist_node	i_hash;
	struct list_head	i_io_list;
	unsigned long		i_state;
	void			*i_private;
	const struct file_operations *i_fop;
};

struct dentry {
	struct list_head	d_child;
	struct dentry		*d_parent;
	struct inode		*d_inode;
	unsigned int		d_flags;
	int			d_count;
	int			d_seq;
	unsigned int		d_hash;
	struct dentry		*d_hash_next;
	struct dentry		*d_hash_prev;
	struct list_head	d_lru;
	spinlock_t		d_lock;
	int			d_lock_count;
	unsigned int		d_lock_flags;
	struct dentry		*d_lock_next;
	struct dentry		*d_lock_prev;
	struct dentry		*d_lock_head;
	struct dentry		*d_lock_tail;
};

static inline struct inode *d_inode(const struct dentry *dentry)
{
	return dentry ? dentry->d_inode : NULL;
}

struct file_operations {
	unsigned int fop_flags;
	struct module	*owner;
	loff_t		(*llseek)(struct file *, loff_t, int);
	ssize_t		(*read)(struct file *, char __user *, size_t, __kernel_off_t *);
	ssize_t		(*write)(struct file *, const char __user *, size_t, __kernel_off_t *);
	ssize_t		(*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t		(*write_iter)(struct kiocb *, struct iov_iter *);
	int		(*open)(struct inode *, struct file *);
	int		(*release)(struct inode *, struct file *);
	int		(*mmap)(struct file *, struct vm_area_struct *);
	unsigned long	(*get_unmapped_area)(struct file *, unsigned long,
					     unsigned long, unsigned long,
					     unsigned long);
#ifdef __poll_t
	__poll_t	(*poll)(struct file *, struct poll_table_struct *);
#else
	unsigned int	(*poll)(struct file *, struct poll_table_struct *);
#endif
	long		(*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long		(*compat_ioctl)(struct file *, unsigned int, unsigned long);
	long		(*check_flags)(struct file *, unsigned int);
	int		(*fasync)(int, struct file *, int);
	int		(*flush)(struct file *, fl_owner_t id);
	int		(*fsync)(struct file *, loff_t, loff_t, int datasync);
};

/* mm helpers (runtime: linuxu/src/mm/page.c) */
extern void mmap_read_lock(struct mm_struct *mm);
extern void mmap_read_unlock(struct mm_struct *mm);
extern void mmap_write_lock(struct mm_struct *mm);
extern void mmap_write_unlock(struct mm_struct *mm);

/* file helpers (runtime: linuxu/src/shims/fs.c) */
extern struct file *filp_open(const char *filename, int flags, umode_t mode);
extern int filp_close(struct file *filp, fl_owner_t id);
extern long vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos);
extern long vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos);
extern long vfs_llseek(struct file *file, loff_t offset, int whence);
extern loff_t default_llseek(struct file *file, loff_t offset, int whence);
extern int simple_open(struct inode *inode, struct file *file);

/* fd lifecycle (upstream linux/file.h; used by amdgpu_cs.c / kfd_chardev.c) */
extern int get_unused_fd_flags(unsigned flags);
extern void put_unused_fd(unsigned int fd);
extern void fd_install(unsigned int fd, struct file *file);
extern struct file *fget(unsigned int fd);
extern void fput(struct file *file);
extern struct file *file_clone_open(struct file *file);

/* get_unused_fd cleanup class (vendor 2026 file.h; needs fdget/put_unused_fd above) */
#include <linux/cleanup.h>
DEFINE_CLASS(get_unused_fd, int,
	     if (_T >= 0) put_unused_fd(_T),
	     get_unused_fd_flags(flags), unsigned flags)

extern int vfs_fsync(struct file *file, int datasync);
extern int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync);
extern int vfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
extern int vfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
extern int vfs_fattr_get(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct kstat *stat, unsigned int request_mask);
extern int vfs_getattr(struct mnt_idmap *idmap, const struct path *path,
			struct kstat *stat, unsigned int request_mask,
			unsigned int query_flags);
extern int vfs_statfs(struct dentry *dentry, struct kstatfs *buf);
extern int vfs_lstat(struct dentry *dentry, struct kstat *stat);
extern int vfs_stat(struct dentry *dentry, struct kstat *stat);
extern int vfs_lstatat(struct dentry *dentry, struct kstat *stat);
extern int vfs_statat(struct dentry *dentry, struct kstat *stat);
extern int vfs_fstat(struct file *file, struct kstat *stat);
extern int vfs_newfstat(struct file *file, struct kstat *stat);
extern int vfs_fstatat(int dfd, const char __user *filename, struct kstat *stat, int flags);
extern int vfs_newfstatat(int dfd, const char __user *filename, struct kstat *stat, int flags);

#define compat_ptr_ioctl NULL

/* epoll event bits (uapi/linux/eventpoll.h; the uapi include is not in the
 * driver's include closure, so define the constants here) */
#ifndef EPOLLIN
#define EPOLLIN		0x00000001
#endif
#ifndef EPOLLRDNORM
#define EPOLLRDNORM	0x00000040
#endif

/* anon_inode surface (drm_file.c: anon_inode_getfile) */
extern struct file *anon_inode_getfile(const char *name,
				       const struct file_operations *fops,
				       void *priv, int flags);

/* pid surface (drm_file.c: pid_task(PIDTYPE_TGID, ...)) — the canonical
 * owner is <linux/pid.h>; include it so the enum/inline are in scope. */
#include <linux/pid.h>


extern void *memdup_user(const void *src, size_t size);
/* memdup_array_user / vmemdup_array_user are in <linux/uaccess.h> */
extern void *vmemdup_user(const void *src, size_t size);

/* sysfs/debugfs read helpers (upstream linux/fs.h; used by kfd_debugfs.c —
 * userspace has no user/kernel split, so a plain char * covers both) */
extern ssize_t simple_read_from_buffer(void *to, size_t count,
				       loff_t *ppos, const void *buf,
				       size_t size);
extern ssize_t simple_write_to_buffer(void *to, size_t available,
				       loff_t *ppos, const void *buf,
				       size_t count);


extern struct file *fget(unsigned int fd);
extern struct file *fget_raw(unsigned int fd);

/* file reference counting (upstream linux/file.h) */
extern struct file *get_file(struct file *f);
extern void fput(struct file *f);
static inline void put_file(struct file *f) { fput(f); }



struct file_system_type {
	const char *name;
	int refcnt;
	struct module *owner;
	int (*init_fs_context)(struct fs_context *fc);
	void (*kill_sb)(struct super_block *sb);
};

/* In-process pseudo filesystem; no host filesystem namespace is mounted. */
struct super_block {
	unsigned long s_magic;
	struct file_system_type *s_type;
	unsigned int linuxu_refs;
};

extern int simple_pin_fs(struct file_system_type *fstype,
			 struct vfsmount **mnt, int *count);
extern void simple_unpin_fs(struct file_system_type *fstype,
			    struct vfsmount *mnt, int *count);

struct vfsmount {
	struct dentry *mnt_root;
	struct dentry *mnt_parent;
	char *mnt_name;
	struct super_block *mnt_sb;
};

extern struct inode *alloc_anon_inode(struct super_block *sb);
extern void iput(struct inode *inode);
extern void simple_release_fs(struct vfsmount **mnt, int *count);
extern void kill_anon_super(struct super_block *sb);

static inline const struct file_operations *fops_get(const struct file_operations *fops)
{
	return fops && try_module_get(fops->owner) ? fops : NULL;
}

static inline void fops_put(const struct file_operations *fops)
{
	if (fops)
		module_put(fops->owner);
}

static inline void replace_fops(struct file *file,
				const struct file_operations *fops)
{
	if (!file || !fops)
		return;
	fops_put(file->f_op);
	file->f_op = fops;
}
static inline loff_t noop_llseek(struct file *file, loff_t offset, int whence)
{
	return 0;
}

#define FOP_UNSIGNED_OFFSET 0x0002
#endif /* __LINUX_FS_H */
