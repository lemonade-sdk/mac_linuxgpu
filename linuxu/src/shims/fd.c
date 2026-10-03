/* Descriptor tables. Each linuxu process has one files_struct shared by its
 * thread tasks (task->files); a task without one uses the kernel table.
 * An installed descriptor owns one file reference. Like Linux, the lowest
 * free descriptor is allocated and a table grows on demand. */
#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <rt/task.h>
#include <rt/chrdev.h>

#define LINUXU_INIT_FDS 64
struct fd_slot { struct file *file; bool reserved; };
struct files_struct {
	int count;
	pthread_mutex_t lock;
	unsigned int max_fds;
	struct fd_slot *fdt;
	struct fd_slot initial[LINUXU_INIT_FDS];
};
/* The kernel's own table: kernel threads and threads outside a process. */
static struct files_struct init_files = {
	.count = INT_MAX,
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.max_fds = LINUXU_INIT_FDS,
	.fdt = init_files.initial,
};
struct anonymous_file {
	struct file file;
	struct inode inode;
	struct address_space mapping;
};

struct files_struct *linuxu_current_files(void)
{
	struct task_struct *task = linuxu_current_task_peek();
	return task && task->files ? task->files : &init_files;
}
struct files_struct *linuxu_files_alloc(void)
{
	struct files_struct *files = calloc(1, sizeof(*files));
	if (!files) return NULL;
	files->count = 1;
	pthread_mutex_init(&files->lock, NULL);
	files->max_fds = LINUXU_INIT_FDS;
	files->fdt = files->initial;
	return files;
}
struct files_struct *linuxu_files_get(struct files_struct *files)
{
	if (files && files != &init_files)
		__atomic_add_fetch(&files->count, 1, __ATOMIC_RELAXED);
	return files;
}
unsigned int linuxu_files_close_all(struct files_struct *files)
{
	unsigned int closed = 0;
	if (!files) return 0;
	for (;;) {
		struct file *file = NULL;
		pthread_mutex_lock(&files->lock);
		for (unsigned int i = 0; i < files->max_fds; ++i) {
			if (files->fdt[i].file) {
				file = files->fdt[i].file;
				files->fdt[i].file = NULL;
			}
			files->fdt[i].reserved = false;
			if (file) break;
		}
		pthread_mutex_unlock(&files->lock);
		/* Release callbacks run unlocked and may open or close fds. */
		if (!file) return closed;
		fput(file);
		closed++;
	}
}
unsigned int linuxu_files_count(struct files_struct *files)
{
	unsigned int count = 0;
	if (!files) return 0;
	pthread_mutex_lock(&files->lock);
	for (unsigned int i = 0; i < files->max_fds; ++i)
		if (files->fdt[i].file) count++;
	pthread_mutex_unlock(&files->lock);
	return count;
}
void linuxu_files_put(struct files_struct *files)
{
	if (!files || files == &init_files) return;
	if (__atomic_sub_fetch(&files->count, 1, __ATOMIC_ACQ_REL)) return;
	linuxu_files_close_all(files);
	if (files->fdt != files->initial) free(files->fdt);
	pthread_mutex_destroy(&files->lock);
	free(files);
}
/* Double the table under its lock (Linux expand_files). */
static bool expand_files_locked(struct files_struct *files)
{
	unsigned int size = files->max_fds * 2;
	if (files->max_fds >= LINUXU_NR_OPEN) return false;
	if (size > LINUXU_NR_OPEN) size = LINUXU_NR_OPEN;
	struct fd_slot *table = calloc(size, sizeof(*table));
	if (!table) return false;
	memcpy(table, files->fdt, files->max_fds * sizeof(*table));
	if (files->fdt != files->initial) free(files->fdt);
	files->fdt = table;
	files->max_fds = size;
	return true;
}

int get_unused_fd_flags(unsigned flags)
{
	(void)flags;
	struct files_struct *files = linuxu_current_files();
	int fd = -EMFILE;
	pthread_mutex_lock(&files->lock);
	for (unsigned i = 0;; ++i) {
		if (i == files->max_fds && !expand_files_locked(files)) break;
		if (files->fdt[i].reserved) continue;
		files->fdt[i].reserved = true;
		fd = i;
		break;
	}
	pthread_mutex_unlock(&files->lock);
	return fd;
}
void put_unused_fd(unsigned int fd)
{
	struct files_struct *files = linuxu_current_files();
	pthread_mutex_lock(&files->lock);
	if (fd < files->max_fds && !files->fdt[fd].file)
		files->fdt[fd].reserved = false;
	pthread_mutex_unlock(&files->lock);
}
void fd_install(unsigned int fd, struct file *file)
{
	struct files_struct *files = linuxu_current_files();
	bool installed = false;
	pthread_mutex_lock(&files->lock);
	if (fd < files->max_fds && files->fdt[fd].reserved &&
	    !files->fdt[fd].file && !IS_ERR_OR_NULL(file)) {
		files->fdt[fd].file = file;
		installed = true;
	}
	pthread_mutex_unlock(&files->lock);
	/* Invalid publication must not overwrite an existing owner's reference. */
	if (!installed && !IS_ERR_OR_NULL(file)) fput(file);
}
struct file *get_file(struct file *file)
{
	if (IS_ERR_OR_NULL(file)) return NULL;
	int count = __atomic_load_n(&file->f_count, __ATOMIC_RELAXED);
	for (;;) {
		if (count <= 0) return NULL;
		if (count == INT_MAX) return file; /* Saturate instead of wrapping. */
		if (__atomic_compare_exchange_n(&file->f_count, &count, count + 1,
			false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return file;
	}
}
struct file *fget(unsigned int fd)
{
	struct files_struct *files = linuxu_current_files();
	struct file *file = NULL;
	pthread_mutex_lock(&files->lock);
	if (fd < files->max_fds) file = get_file(files->fdt[fd].file);
	pthread_mutex_unlock(&files->lock);
	return file;
}
struct file *fget_raw(unsigned int fd) { return fget(fd); }
void fput(struct file *file)
{
	if (IS_ERR_OR_NULL(file)) return;
	int count = __atomic_load_n(&file->f_count, __ATOMIC_RELAXED);
	for (;;) {
		if (count <= 0 || count == INT_MAX) return;
		if (__atomic_compare_exchange_n(&file->f_count, &count, count - 1,
			false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) break;
	}
	if (count != 1) return;
	bool allocated = file->linuxu_allocated;
	if (file->f_op && file->f_op->release)
		file->f_op->release(file->f_inode, file);
	if (allocated) kfree(file);
}
int close_fd(unsigned int fd)
{
	struct files_struct *files = linuxu_current_files();
	struct file *file = NULL;
	pthread_mutex_lock(&files->lock);
	if (fd < files->max_fds && files->fdt[fd].file) {
		file = files->fdt[fd].file;
		files->fdt[fd].file = NULL;
		files->fdt[fd].reserved = false;
	}
	pthread_mutex_unlock(&files->lock);
	if (!file) return -EBADF;
	fput(file);
	return 0;
}
struct file *anon_inode_getfile(const char *name,
	const struct file_operations *fops, void *priv, int flags)
{
	(void)name;
	if (!fops) return ERR_PTR(-EINVAL);
	struct anonymous_file *anon = kzalloc(sizeof(*anon), GFP_KERNEL);
	if (!anon) return ERR_PTR(-ENOMEM);
	anon->file.f_op = fops;
	anon->file.f_inode = &anon->inode;
	anon->file.f_mapping = &anon->mapping;
	anon->file.f_flags = flags;
	anon->file.f_mode = (flags & O_ACCMODE) == O_RDONLY ? FMODE_READ :
		(flags & O_ACCMODE) == O_WRONLY ? FMODE_WRITE : FMODE_READ | FMODE_WRITE;
	anon->file.private_data = priv;
	anon->file.f_count = 1;
	anon->file.linuxu_allocated = true;
	anon->inode.i_mapping = &anon->mapping;
	anon->inode.i_mode = S_IFREG | S_IRUSR | S_IWUSR;
	anon->inode.i_count.count.counter = 1;
	anon->mapping.host = &anon->inode;
	return &anon->file;
}
/* A character-device open file description (Linux chrdev_open followed by
 * do_dentry_open): its own inode carries i_rdev, as the device node's inode
 * does. Freed with the file by fput (linuxu_allocated). */
struct chrdev_file {
	struct file file;
	struct inode inode;
	struct address_space mapping;
};
int linuxu_chrdev_open(dev_t dev, int flags, struct file **out)
{
	const struct file_operations *fops;
	struct chrdev_file *cf;
	int r;

	if (!out) return -EINVAL;
	*out = NULL;
	r = linuxu_chrdev_lookup(dev, &fops);
	if (r) return r;
	cf = kzalloc(sizeof(*cf), GFP_KERNEL);
	if (!cf) return -ENOMEM;
	cf->inode.i_mode = S_IFCHR | S_IRUSR | S_IWUSR;
	cf->inode.i_rdev = dev;
	cf->inode.i_mapping = &cf->mapping;
	cf->inode.i_count.count.counter = 1;
	cf->inode.i_fop = fops;
	cf->mapping.host = &cf->inode;
	cf->file.f_inode = &cf->inode;
	cf->file.f_mapping = &cf->mapping;
	cf->file.f_flags = flags;
	cf->file.f_mode = (flags & O_ACCMODE) == O_RDONLY ? FMODE_READ :
		(flags & O_ACCMODE) == O_WRONLY ? FMODE_WRITE : FMODE_READ | FMODE_WRITE;
	cf->file.f_count = 1;
	cf->file.linuxu_allocated = true;
	cf->file.f_op = fops_get(fops);
	if (cf->file.f_op && cf->file.f_op->open) {
		r = cf->file.f_op->open(&cf->inode, &cf->file);
		if (r) {
			fops_put(cf->file.f_op);
			kfree(cf);
			return r;
		}
	}
	*out = &cf->file;
	return 0;
}
struct file *file_clone_open(struct file *file)
{
	(void)file;
	/* A new open description requires the original filesystem's open path. */
	return ERR_PTR(-EOPNOTSUPP);
}
void __f_lock_pos(struct file *file)
{
	while (__atomic_exchange_n(&file->f_pos_lock, 1, __ATOMIC_ACQUIRE)) {
#if defined(__aarch64__)
		__asm__ volatile("yield" ::: "memory");
#else
		__asm__ volatile("pause" ::: "memory");
#endif
	}
}
void __f_unlock_pos(struct file *file)
{
	__atomic_store_n(&file->f_pos_lock, 0, __ATOMIC_RELEASE);
}
struct device_memory *devm_register_device_memory(struct device *dev)
{
	(void)dev;
	return ERR_PTR(-ENODEV);
}
void devm_unregister_device_memory(struct device *dev, struct device_memory *dm)
{ (void)dev; (void)dm; }
