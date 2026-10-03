#include <assert.h>
#include <linux/fs.h>
#include <rt/chrdev.h>

static const struct file_operations drm_ops = { 0 };
static const struct file_operations kfd_ops = { .owner = THIS_MODULE };

static void absent(dev_t number)
{
	const struct file_operations *found = &drm_ops;
	assert(linuxu_chrdev_lookup(number, &found) == -ENXIO);
	assert(found == NULL);
}

int main(void)
{
	const struct file_operations *found = NULL;
	struct inode inode = { 0 };
	struct dentry dentry = { 0 };
	struct file open_file = { .f_op = &drm_ops };
	dev_t allocated = 0;
	int kfd_major;

	assert(d_inode(NULL) == NULL);
	dentry.d_inode = &inode;
	assert(d_inode(&dentry) == &inode);
	inode.i_rdev = MKDEV(226, 19);
	assert(iminor(&inode) == 19);
	assert(fops_get(&drm_ops) == &drm_ops);
	replace_fops(&open_file, fops_get(&kfd_ops));
	assert(open_file.f_op == &kfd_ops);
	fops_put(open_file.f_op);

	assert(register_chrdev(226, "drm", &drm_ops) == 0);
	assert(linuxu_chrdev_lookup(MKDEV(226, 0), &found) == 0);
	assert(found == &drm_ops);
	assert(linuxu_chrdev_lookup(MKDEV(226, 255), &found) == 0);
	absent(MKDEV(226, 256));
	assert(register_chrdev(226, "duplicate", &kfd_ops) == -EBUSY);
	assert(register_chrdev(512, "invalid", &kfd_ops) == -EINVAL);
	assert(register_chrdev(0, "missing fops", NULL) == -EINVAL);

	kfd_major = register_chrdev(0, "kfd", &kfd_ops);
	assert(kfd_major == 254);
	{
		/* /proc/devices: the major registered under a name. */
		unsigned int major = 0;
		assert(linuxu_chrdev_find("kfd", &major) == 0 && major == 254);
		assert(linuxu_chrdev_find("drm", &major) == 0 && major == 226);
		assert(linuxu_chrdev_find("absent", &major) == -ENXIO);
		assert(linuxu_chrdev_find(NULL, &major) == -EINVAL);
	}
	assert(linuxu_chrdev_lookup(MKDEV(kfd_major, 0), &found) == 0);
	assert(found == &kfd_ops);
	assert(register_chrdev(0, "second", &kfd_ops) == 253);
	unregister_chrdev(253, "second");
	unregister_chrdev(kfd_major, "kfd");
	absent(MKDEV(kfd_major, 0));
	assert(register_chrdev(0, "reused", &kfd_ops) == 254);
	unregister_chrdev(254, "reused");

	assert(register_chrdev_region(MKDEV(240, 4), 3, "reserved") == 0);
	absent(MKDEV(240, 4)); /* range reservation has no file operations */
	assert(register_chrdev_region(MKDEV(240, 6), 2, "overlap") == -EBUSY);
	assert(register_chrdev_region(MKDEV(240, 7), 2, "disjoint") == 0);
	unregister_chrdev_region(MKDEV(240, 4), 3);
	unregister_chrdev_region(MKDEV(240, 7), 2);

	assert(alloc_chrdev_region(&allocated, 12, 2, "allocation") == 0);
	assert(MAJOR(allocated) == 254 && MINOR(allocated) == 12);
	absent(allocated);
	assert(alloc_chrdev_region(&allocated, MINORMASK, 2, "invalid") == -EINVAL);
	unregister_chrdev_region(MKDEV(254, 12), 2);

	assert(register_chrdev_region(MKDEV(230, 0), 2, "block") == 0);
	assert(register_chrdev_region(MKDEV(229, MINORMASK), 3,
				      "must roll back") == -EBUSY);
	assert(register_chrdev_region(MKDEV(229, MINORMASK), 1,
				      "rolled back") == 0);
	unregister_chrdev_region(MKDEV(229, MINORMASK), 1);
	unregister_chrdev_region(MKDEV(230, 0), 2);
	assert(register_chrdev_region(MKDEV(511, MINORMASK), 2,
				      "out of majors") == -EINVAL);

	unregister_chrdev(226, "drm");
	absent(MKDEV(226, 0));
	assert(register_chrdev(226, "drm", &drm_ops) == 0);
	unregister_chrdev(226, "drm");
	return 0;
}
