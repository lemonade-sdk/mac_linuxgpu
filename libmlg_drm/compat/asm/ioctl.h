/* Linux ioctl number encoding (include/uapi/asm-generic/ioctl.h), for
 * building Linux uapi headers in a macOS process. The Linux and BSD
 * encodings differ (direction bits and size width), and the Linux kernel
 * the requests reach decodes the Linux one, so the BSD macros a macOS
 * header may have defined are replaced within the translation unit. */
#ifndef MLG_COMPAT_ASM_IOCTL_H
#define MLG_COMPAT_ASM_IOCTL_H

#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#undef _IOC_DIR
#undef _IOC_TYPE
#undef _IOC_NR
#undef _IOC_SIZE
#undef _IOC_NONE
#undef _IOC_READ
#undef _IOC_WRITE

#define _IOC_NRBITS	8
#define _IOC_TYPEBITS	8
#define _IOC_SIZEBITS	14
#define _IOC_DIRBITS	2

#define _IOC_NRMASK	((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK	((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK	((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK	((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT	0
#define _IOC_TYPESHIFT	(_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT	(_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT	(_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE	0U
#define _IOC_WRITE	1U
#define _IOC_READ	2U

#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_TYPECHECK(t) (sizeof(t))

#define _IO(type, nr)		_IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)	_IOC(_IOC_READ, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOW(type, nr, size)	_IOC(_IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOWR(type, nr, size)	_IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))

#define _IOC_DIR(nr)	(((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)	(((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)	(((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)	(((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#endif
