/* linuxu: SHIM (third_party/linux/include/linux/chardev.h)
 * Character-device registration surface. The compute dext registers the KFD
 * device via an OS-layer path (P2); the registration calls are declared here
 * and resolved at link time. */
#ifndef _LINUX_CHARDEV_H
#define _LINUX_CHARDEV_H

#include <linux/types.h>
#include <linux/kdev_t.h>

struct file_operations;

extern int register_chrdev(unsigned int major, const char *name,
			   const struct file_operations *fops);
extern void unregister_chrdev(unsigned int major, const char *name);
extern int register_chrdev_region(dev_t from, unsigned count, const char *name);
extern void unregister_chrdev_region(dev_t from, unsigned count);
extern int alloc_chrdev_region(dev_t *dev, unsigned int baseminor,
			       unsigned int count, const char *name);
extern void unregister_chrdev_region(dev_t from, unsigned int count);

#endif /* _LINUX_CHARDEV_H */
