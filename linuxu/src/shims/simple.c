/* linuxu: host shim — fs helpers the pm code references.
 * Host build compiles .o only (no link against a real kernel), so
 * the bodies are minimal placeholders. */

#include <linux/fs.h>
#include <linux/types.h>

loff_t default_llseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	if (whence == 0)
		return offset;
	return -1;
}

int simple_open(struct inode *inode, struct file *file)
{
	(void)inode; (void)file;
	return 0;
}

ssize_t simple_read_from_buffer(void __user *to, size_t count,
				loff_t *ppos, const void *from,
				size_t available)
{
	(void)to; (void)count; (void)ppos; (void)from; (void)available;
	return 0;
}

ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
			       const void __user *from, size_t count)
{
	(void)to; (void)available; (void)ppos; (void)from; (void)count;
	return 0;
}

ssize_t kernel_write(struct file *file, const void *buf, size_t count,
		     loff_t *pos)
{
	(void)file; (void)buf; (void)count; (void)pos;
	return 0;
}
