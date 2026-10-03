/* In-memory debugfs tree for upstream DRM diagnostics. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#define DBGS_MAX_ENTRIES 4096

struct dbgfs_entry {
	char *name;
	struct dentry *parent;
	void *data;
	struct dentry *dentry;
	struct inode *inode;
	int used;
};

static struct dbgfs_entry dbgfs_entries[DBGS_MAX_ENTRIES];
static struct dentry dbgfs_dummy_pool[DBGS_MAX_ENTRIES];
static pthread_mutex_t dbgfs_lock = PTHREAD_MUTEX_INITIALIZER;

static struct dentry *dbgfs_create_(const char *name, umode_t mode,
				    struct dentry *parent, void *data)
{
	int i;
	char *owned_name;
	struct inode *inode;

	owned_name = strdup(name ? name : "");
	inode = calloc(1, sizeof(*inode));
	if (!owned_name || !inode) {
		free(owned_name);
		free(inode);
		return NULL;
	}
	pthread_mutex_lock(&dbgfs_lock);
	if (parent) {
		int found = 0;
		for (i = 0; i < DBGS_MAX_ENTRIES; i++)
			if (dbgfs_entries[i].used && dbgfs_entries[i].dentry == parent) {
				found = 1;
				break;
			}
		if (!found) {
			pthread_mutex_unlock(&dbgfs_lock);
			free(owned_name);
			free(inode);
			return NULL;
		}
	}
	for (i = 0; i < DBGS_MAX_ENTRIES; i++) {
		if (!dbgfs_entries[i].used) {
			struct dentry *dentry = &dbgfs_dummy_pool[i];
			memset(dentry, 0, sizeof(*dentry));
			inode->i_mode = mode & (S_IFDIR | S_IFLNK | S_IFREG) ?
				mode : (mode | S_IFREG);
			inode->i_private = data;
			dentry->d_inode = inode;
			dentry->d_parent = parent ? parent : dentry;
			dentry->d_count = 1;
			dbgfs_entries[i].name = owned_name;
			dbgfs_entries[i].parent = parent;
			dbgfs_entries[i].data = data;
			dbgfs_entries[i].dentry = dentry;
			dbgfs_entries[i].inode = inode;
			dbgfs_entries[i].used = 1;
			pthread_mutex_unlock(&dbgfs_lock);
			return dentry;
		}
	}
	pthread_mutex_unlock(&dbgfs_lock);
	free(owned_name);
	free(inode);
	return NULL;
}

/* Caller holds dbgfs_lock; children are removed before their parent. */
static void dbgfs_remove_entry_locked(struct dentry *dentry)
{
	int i;

	if (!dentry)
		return;
	for (i = 0; i < DBGS_MAX_ENTRIES; i++) {
		if (dbgfs_entries[i].used && dbgfs_entries[i].parent == dentry)
			dbgfs_remove_entry_locked(dbgfs_entries[i].dentry);
	}
	for (i = 0; i < DBGS_MAX_ENTRIES; i++)
		if (dbgfs_entries[i].used && dbgfs_entries[i].dentry == dentry) {
			free(dbgfs_entries[i].name);
			free(dbgfs_entries[i].inode);
			memset(dbgfs_entries[i].dentry, 0,
			       sizeof(*dbgfs_entries[i].dentry));
			memset(&dbgfs_entries[i], 0, sizeof(dbgfs_entries[i]));
			return;
		}
}

/* ---- lookup / remove ---- */
struct dentry *debugfs_lookup(const char *name, struct dentry *parent)
{
	int i;

	pthread_mutex_lock(&dbgfs_lock);
	for (i = 0; i < DBGS_MAX_ENTRIES; i++) {
		if (dbgfs_entries[i].used &&
		    dbgfs_entries[i].parent == parent &&
		    strcmp(dbgfs_entries[i].name, name) == 0)
			break;
	}
	pthread_mutex_unlock(&dbgfs_lock);
	return i < DBGS_MAX_ENTRIES ? dbgfs_entries[i].dentry : NULL;
}

void debugfs_remove(struct dentry *dentry)
{
	pthread_mutex_lock(&dbgfs_lock);
	dbgfs_remove_entry_locked(dentry);
	pthread_mutex_unlock(&dbgfs_lock);
}

void debugfs_lookup_and_remove(const char *name, struct dentry *parent)
{
	struct dentry *d = debugfs_lookup(name, parent);

	debugfs_remove(d);
}

/* ---- create family ---- */
struct dentry *debugfs_create_file_full(const char *name, umode_t mode,
					struct dentry *parent, void *data,
					const void *aux,
					const struct file_operations *fops)
{
	struct dentry *dentry = dbgfs_create_(name, mode, parent, data);
	(void)aux;
	if (dentry)
		d_inode(dentry)->i_fop = fops;
	return dentry;
}

struct dentry *debugfs_create_file_short(const char *name, umode_t mode,
					 struct dentry *parent, void *data,
					 const void *aux,
					 const struct debugfs_short_fops *fops)
{
	(void)aux; (void)fops;
	return dbgfs_create_(name, mode, parent, data);
}

struct dentry *debugfs_create_file_unsafe(const char *name, umode_t mode,
					  struct dentry *parent, void *data,
					  const struct file_operations *fops)
{
	struct dentry *dentry = dbgfs_create_(name, mode, parent, data);
	if (dentry)
		d_inode(dentry)->i_fop = fops;
	return dentry;
}

void debugfs_create_file_size(const char *name, umode_t mode,
			      struct dentry *parent, void *data,
			      const struct file_operations *fops,
			      loff_t file_size)
{
	struct dentry *dentry = dbgfs_create_(name, mode, parent, data);
	if (dentry) {
		d_inode(dentry)->i_fop = fops;
		d_inode(dentry)->i_size = file_size;
	}
}

struct dentry *debugfs_create_dir(const char *name, struct dentry *parent)
{
	return dbgfs_create_(name, S_IFDIR | 0755, parent, NULL);
}

struct dentry *debugfs_create_symlink(const char *name,
				      struct dentry *parent,
				      const char *dest)
{
	(void)dest;
	return dbgfs_create_(name, S_IFLNK | 0777, parent, NULL);
}

struct dentry *debugfs_create_automount(const char *name,
					struct dentry *parent,
					debugfs_automount_t f,
					void *data)
{
	(void)f;
	return dbgfs_create_(name, 0, parent, data);
}

void *debugfs_get_aux(const struct file *file)
{
	(void)file;
	return NULL;
}

int debugfs_file_get(struct dentry *dentry)
{
	(void)dentry;
	return 0;
}

void debugfs_file_put(struct dentry *dentry)
{
	(void)dentry;
}

ssize_t debugfs_attr_read(struct file *file, char *buf,
			  size_t len, loff_t *ppos)
{
	(void)file; (void)buf; (void)len; (void)ppos;
	return 0;
}

ssize_t debugfs_attr_write(struct file *file, const char *buf,
			   size_t len, loff_t *ppos)
{
	(void)file; (void)buf; (void)ppos;
	return (ssize_t)len;
}

ssize_t debugfs_attr_write_signed(struct file *file, const char *buf,
				  size_t len, loff_t *ppos)
{
	return debugfs_attr_write(file, buf, len, ppos);
}

int debugfs_change_name(struct dentry *dentry, const char *fmt, ...)
{
	char buf[256];
	char *copy;
	va_list args;
	int n, i;
	if (!dentry || !fmt)
		return -1;
	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	copy = strdup(buf);
	if (!copy)
		return -1;
	pthread_mutex_lock(&dbgfs_lock);
	for (i = 0; i < DBGS_MAX_ENTRIES; i++)
		if (dbgfs_entries[i].used && dbgfs_entries[i].dentry == dentry) {
			free(dbgfs_entries[i].name);
			dbgfs_entries[i].name = copy;
			pthread_mutex_unlock(&dbgfs_lock);
			return 0;
		}
	pthread_mutex_unlock(&dbgfs_lock);
	free(copy);
	return -1;
}

/* ---- typed create helpers ---- */
#define DBGFS_TYPED_IMPL(fn, type)					\
	void fn(const char *name, umode_t mode, struct dentry *parent,	\
	       type *value)						\
	{ dbgfs_create_(name, mode, parent, (void *)(uintptr_t)value); }

DBGFS_TYPED_IMPL(debugfs_create_u8, u8)
DBGFS_TYPED_IMPL(debugfs_create_u16, u16)
DBGFS_TYPED_IMPL(debugfs_create_u32, u32)
DBGFS_TYPED_IMPL(debugfs_create_u64, u64)
DBGFS_TYPED_IMPL(debugfs_create_ulong, unsigned long)
DBGFS_TYPED_IMPL(debugfs_create_x8, u8)
DBGFS_TYPED_IMPL(debugfs_create_x16, u16)
DBGFS_TYPED_IMPL(debugfs_create_x32, u32)
DBGFS_TYPED_IMPL(debugfs_create_x64, u64)
DBGFS_TYPED_IMPL(debugfs_create_size_t, size_t)
DBGFS_TYPED_IMPL(debugfs_create_atomic_t, atomic_t)
DBGFS_TYPED_IMPL(debugfs_create_bool, bool)

void debugfs_create_str(const char *name, umode_t mode,
			struct dentry *parent, char **value)
{
	dbgfs_create_(name, mode, parent, (void *)value);
}

struct dentry *debugfs_create_blob(const char *name, umode_t mode,
				   struct dentry *parent,
				   struct debugfs_blob_wrapper *blob)
{
	return dbgfs_create_(name, mode, parent, (void *)blob);
}

void debugfs_create_regset32(const char *name, umode_t mode,
			     struct dentry *parent,
			     struct debugfs_regset32 *regset)
{
	dbgfs_create_(name, mode, parent, (void *)regset);
}

void debugfs_print_regs32(struct seq_file *s, const struct debugfs_reg32 *regs,
			  int nregs, void __iomem *base, char *prefix)
{
	(void)s; (void)regs; (void)nregs; (void)base; (void)prefix;
}

void debugfs_create_u32_array(const char *name, umode_t mode,
			      struct dentry *parent,
			      struct debugfs_u32_array *array)
{
	dbgfs_create_(name, mode, parent, (void *)array);
}

void debugfs_create_devm_seqfile(struct device *dev, const char *name,
				 struct dentry *parent,
				 int (*read_fn)(struct seq_file *s,
						void *data))
{
	(void)dev; (void)read_fn;
	dbgfs_create_(name, 0, parent, NULL);
}

bool debugfs_initialized(void)
{
	return true;
}

ssize_t debugfs_read_file_bool(struct file *file, char *user_buf,
			       size_t count, loff_t *ppos)
{
	(void)file; (void)ppos;
	if (user_buf && count)
		user_buf[0] = '0';
	return count ? 1 : 0;
}

ssize_t debugfs_write_file_bool(struct file *file, const char *user_buf,
				size_t count, loff_t *ppos)
{
	(void)file; (void)user_buf; (void)ppos;
	return (ssize_t)count;
}

ssize_t debugfs_read_file_str(struct file *file, char *user_buf,
			      size_t count, loff_t *ppos)
{
	(void)file; (void)ppos;
	if (user_buf && count)
		user_buf[0] = '\0';
	return 0;
}

/* ---- test hook: read a created file's data pointer ---- */
void *linuxu_dbgfs_read(const char *name, struct dentry *parent)
{
	int i;

	pthread_mutex_lock(&dbgfs_lock);
	for (i = 0; i < DBGS_MAX_ENTRIES; i++) {
		if (dbgfs_entries[i].used &&
		    dbgfs_entries[i].parent == parent &&
		    strcmp(dbgfs_entries[i].name, name) == 0)
			break;
	}
	pthread_mutex_unlock(&dbgfs_lock);
	return i < DBGS_MAX_ENTRIES ? dbgfs_entries[i].data : NULL;
}
