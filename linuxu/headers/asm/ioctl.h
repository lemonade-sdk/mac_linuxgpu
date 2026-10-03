/* linuxu: SHIM (vendor arch/x86/include/asm/ioctl.h)
 *
 * ioctl encoding: use the Linux _IOC macros (not macOS's ioccom),
 * because the driver's ioctls must match the Linux KFD/DRM ABI.
 */
#ifndef _ASM_IOCTL_H
#define _ASM_IOCTL_H

#include <linux/types.h>

#define _IOC_NRBITS		8
#define _IOC_TYPEBITS		8
#define _IOC_SIZEBITS		14
#define _IOC_DIRBITS		2

#define _IOC_NRMASK		((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK		((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK		((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK		((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT		0
#define _IOC_TYPESHIFT		(_IOC_NRBITS)
#define _IOC_SIZESHIFT		(_IOC_NRBITS + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT		(_IOC_NRBITS + _IOC_TYPEBITS + _IOC_SIZEBITS)

/* Direction bits (Linux) */
#define _IOC_READ		2U
#define _IOC_WRITE		1U
#define _IOC_NONE		0U

#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | \
	 ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | \
	 ((size) << _IOC_SIZESHIFT))

#define _IOC_TYPECHECK(t)	(sizeof(t))
#define _IO(type, nr)		_IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)	_IOC(_IOC_READ, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOW(type, nr, size)	_IOC(_IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOWR(type, nr, size)	_IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))

/* Direction masks (uapi/asm-generic/ioctl.h: IOC_IN/IOC_OUT are the bit
 * masks for the direction field, not booleans). */
#define IOC_IN			(_IOC_WRITE << _IOC_DIRSHIFT)
#define IOC_OUT			(_IOC_READ << _IOC_DIRSHIFT)

/* Field accessors */
#define IOC_SIZE(nr)		(_IOC_SIZE(nr))
#define IOC_DIR(nr)		(_IOC_DIR(nr))
#define IOC_TYPE(nr)		(_IOC_TYPE(nr))
#define IOC_NR(nr)		(_IOC_NR(nr))

#define _IOC_SIZE(nr)		(((unsigned int)(nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)
#define _IOC_DIR(nr)		(((unsigned int)(nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)		(((unsigned int)(nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)		(((unsigned int)(nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)

#define ioctl_is_valid(nr)	1

#endif /* _ASM_IOCTL_H */
