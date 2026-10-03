/* linuxu: SHIM (third_party/linux/include/linux/kdev_t.h) */
#ifndef __LINUX_KDEV_T_H
#define __LINUX_KDEV_T_H
#include <linux/types.h>
#define MINORBITS 20
#define MAJORBITS 12
#define MINORMASK ((1U << MINORBITS) - 1)
#define MKDEV(ma, mi) (((ma) << 20) | (mi))
#define MAJOR(dev) ((dev) >> 20)
#define MINOR(dev) ((dev) & MINORMASK)
#define iminor(inode) MINOR((inode)->i_rdev)

static inline dev_t old_encode_dev(dev_t dev)
{
	return dev;
}
#endif
