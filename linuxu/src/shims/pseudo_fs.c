/* Anonymous DRM inode/mapping ownership inside the driver process. */
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pseudo_fs.h>

static pthread_mutex_t mount_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long next_inode = 1;

struct anonymous_inode {
	struct inode inode;
	struct address_space mapping;
};

struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
	struct pseudo_fs_context *ctx;
	if (!fc || fc->fs_private)
		return NULL;
	ctx = calloc(1, sizeof(*ctx));
	if (ctx) {
		ctx->magic = magic;
		fc->fs_private = ctx;
	}
	return ctx;
}

void kill_anon_super(struct super_block *sb)
{
	free(sb);
}

static void super_put(struct super_block *sb)
{
	if (__atomic_sub_fetch(&sb->linuxu_refs, 1, __ATOMIC_ACQ_REL) == 0) {
		if (sb->s_type->kill_sb)
			sb->s_type->kill_sb(sb);
		else
			kill_anon_super(sb);
	}
}

struct inode *alloc_anon_inode(struct super_block *sb)
{
	struct anonymous_inode *anon;
	if (!sb)
		return ERR_PTR(-EINVAL);
	anon = calloc(1, sizeof(*anon));
	if (!anon)
		return ERR_PTR(-ENOMEM);
	__atomic_add_fetch(&sb->linuxu_refs, 1, __ATOMIC_RELAXED);
	refcount_set(&anon->inode.i_count, 1);
	anon->inode.i_sb = sb;
	anon->inode.i_mapping = &anon->mapping;
	anon->inode.i_mode = S_IFREG | S_IRUSR | S_IWUSR;
	anon->inode.i_ino = __atomic_fetch_add(&next_inode, 1, __ATOMIC_RELAXED);
	anon->inode.i_nlink = 1;
	INIT_LIST_HEAD(&anon->inode.i_io_list);
	INIT_HLIST_NODE(&anon->inode.i_hash);
	anon->mapping.host = &anon->inode;
	return &anon->inode;
}

void iput(struct inode *inode)
{
	struct super_block *sb;
	if (!inode)
		return;
	if (__atomic_sub_fetch(&inode->i_count.count.counter, 1, __ATOMIC_ACQ_REL))
		return;
	sb = inode->i_sb;
	free(inode);
	super_put(sb);
}

static void mount_free(struct vfsmount *mnt)
{
	iput(mnt->mnt_root->d_inode);
	free(mnt->mnt_root);
	super_put(mnt->mnt_sb);
	free(mnt);
}

int simple_pin_fs(struct file_system_type *type, struct vfsmount **mnt, int *count)
{
	struct vfsmount *new_mount;
	struct super_block *sb;
	struct pseudo_fs_context *ctx;
	struct fs_context fc = { .fs_type = type };
	int ret = 0;
	if (!type || !type->init_fs_context || !mnt || !count)
		return -EINVAL;
	pthread_mutex_lock(&mount_lock);
	if (*count < 0 || (!!*mnt != (*count > 0)) ||
	    (*mnt && (*mnt)->mnt_sb->s_type != type)) {
		ret = -EINVAL;
		goto out;
	}
	if (*count == INT_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (*mnt) {
		++*count;
		goto out;
	}
	ret = type->init_fs_context(&fc);
	ctx = fc.fs_private;
	if (ret || !ctx) {
		free(ctx);
		if (!ret)
			ret = -EINVAL;
		goto out;
	}
	new_mount = calloc(1, sizeof(*new_mount));
	sb = calloc(1, sizeof(*sb));
	if (!new_mount || !sb) {
		free(new_mount);
		free(sb);
		free(ctx);
		ret = -ENOMEM;
		goto out;
	}
	sb->s_magic = ctx->magic;
	sb->s_type = type;
	sb->linuxu_refs = 1;
	free(ctx);
	new_mount->mnt_sb = sb;
	new_mount->mnt_root = calloc(1, sizeof(*new_mount->mnt_root));
	if (!new_mount->mnt_root) {
		super_put(sb);
		free(new_mount);
		ret = -ENOMEM;
		goto out;
	}
	new_mount->mnt_root->d_inode = alloc_anon_inode(sb);
	if (IS_ERR(new_mount->mnt_root->d_inode)) {
		ret = PTR_ERR(new_mount->mnt_root->d_inode);
		free(new_mount->mnt_root);
		super_put(sb);
		free(new_mount);
		goto out;
	}
	new_mount->mnt_root->d_inode->i_mode = S_IFDIR | S_IRUSR | S_IXUSR;
	new_mount->mnt_root->d_parent = new_mount->mnt_root;
	new_mount->mnt_root->d_count = 1;
	*mnt = new_mount;
	*count = 1;
out:
	pthread_mutex_unlock(&mount_lock);
	return ret;
}

void simple_release_fs(struct vfsmount **mnt, int *count)
{
	struct vfsmount *last = NULL;
	if (!mnt || !count)
		return;
	pthread_mutex_lock(&mount_lock);
	if (*mnt && *count > 0 && --*count == 0) {
		last = *mnt;
		*mnt = NULL;
	}
	pthread_mutex_unlock(&mount_lock);
	if (last)
		mount_free(last);
}
