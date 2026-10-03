#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pseudo_fs.h>

static int initialized, destroyed;
static struct vfsmount *mount;
static int pins;

static int init(struct fs_context *fc)
{
	++initialized;
	return init_pseudo(fc, 0x010203ff) ? 0 : -ENOMEM;
}
static void destroy(struct super_block *sb)
{
	__atomic_add_fetch(&destroyed, 1, __ATOMIC_RELAXED);
	kill_anon_super(sb);
}
static struct file_system_type type = {
	.name = "drm-test", .init_fs_context = init, .kill_sb = destroy,
};
static int fail_init(struct fs_context *fc)
{
	(void)init_pseudo(fc, 1);
	return -EIO;
}
static void *worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < 500; ++i) {
		assert(simple_pin_fs(&type, &mount, &pins) == 0);
		struct inode *inode = alloc_anon_inode(mount->mnt_sb);
		assert(!IS_ERR(inode));
		assert(inode->i_mapping->host == inode);
		iput(inode);
		simple_release_fs(&mount, &pins);
	}
	return NULL;
}
int main(void)
{
	struct file_system_type failing = {
		.name = "fail", .init_fs_context = fail_init,
	};
	assert(IS_ERR(alloc_anon_inode(NULL)));
	assert(simple_pin_fs(NULL, &mount, &pins) == -EINVAL);
	assert(simple_pin_fs(&failing, &mount, &pins) == -EIO);
	assert(!mount && !pins);
	assert(simple_pin_fs(&type, &mount, &pins) == 0);
	struct vfsmount *same = mount;
	assert(mount->mnt_sb->s_magic == 0x010203ff);
	assert(d_inode(mount->mnt_root)->i_mode & S_IFDIR);
	assert(simple_pin_fs(&type, &mount, &pins) == 0);
	assert(mount == same && pins == 2 && initialized == 1);
	assert(simple_pin_fs(&failing, &mount, &pins) == -EINVAL);
	struct inode *a = alloc_anon_inode(mount->mnt_sb);
	struct inode *b = alloc_anon_inode(mount->mnt_sb);
	assert(!IS_ERR(a) && !IS_ERR(b));
	assert(a != b && a->i_mapping != b->i_mapping && a->i_ino != b->i_ino);
	assert(a->i_mapping->host == a && b->i_mapping->host == b);
	assert(a->i_sb == b->i_sb);
	simple_release_fs(&mount, &pins);
	assert(mount == same && pins == 1);
	simple_release_fs(&mount, &pins);
	assert(!mount && !pins && destroyed == 0);
	iput(a);
	assert(destroyed == 0);
	iput(b);
	assert(destroyed == 1);
	simple_release_fs(&mount, &pins);
	assert(!mount && !pins);
	/* Hold one pin so worker access to the shared mount remains valid. */
	assert(simple_pin_fs(&type, &mount, &pins) == 0);
	pthread_t threads[4];
	for (int i = 0; i < 4; ++i)
		assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
	for (int i = 0; i < 4; ++i)
		assert(pthread_join(threads[i], NULL) == 0);
	assert(pins == 1);
	simple_release_fs(&mount, &pins);
	assert(destroyed == 2 && initialized == 2);
	puts("pseudo fs ownership, failure unwind, and concurrent pins: PASS");
}
