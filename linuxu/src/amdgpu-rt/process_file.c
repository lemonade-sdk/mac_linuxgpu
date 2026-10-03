/* Descriptors of a linuxu process (rt/process_file.h): character-device
 * opens and ioctls whose argument blocks live in the process's user memory,
 * as libhsakmt and libdrm issue them on Linux. */
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <rt/chrdev.h>
#include <rt/process_file.h>

int rt_process_open_chrdev(dev_t dev, int flags)
{
	struct file *file;
	int fd, r;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	r = linuxu_chrdev_open(dev, flags, &file);
	if (r) {
		put_unused_fd(fd);
		return r;
	}
	fd_install(fd, file);
	return fd;
}

long rt_process_ioctl(int fd, unsigned int cmd, unsigned long va, void *buf, size_t bytes)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct file *file;
	size_t span = PAGE_ALIGN(bytes);
	void *kbuf;
	long ret;

	if (!mm || !bytes || (va & ~PAGE_MASK))
		return -EINVAL;
	file = fget(fd);
	if (!file)
		return -EBADF;
	kbuf = kzalloc(span, GFP_KERNEL);
	vma = vm_area_alloc(mm);
	if (!kbuf || !vma) {
		kfree(kbuf);
		vm_area_free(vma);
		fput(file);
		return -ENOMEM;
	}
	memcpy(kbuf, buf, bytes);
	vma->vm_start = va;
	vma->vm_end = va + span;
	vma->vm_flags = VM_READ | VM_WRITE;
	vma->linuxu_kaddr = kbuf;
	mmap_write_lock(mm);
	ret = linuxu_mm_insert_vma(mm, vma);
	mmap_write_unlock(mm);
	if (!ret) {
		ret = file->f_op && file->f_op->unlocked_ioctl ?
			file->f_op->unlocked_ioctl(file, cmd, va) : -ENOTTY;
		mmap_write_lock(mm);
		linuxu_mm_remove_vma(mm, vma);
		mmap_write_unlock(mm);
		memcpy(buf, kbuf, bytes);
	}
	vm_area_free(vma);
	kfree(kbuf);
	fput(file);
	return ret;
}
