/* linuxu: EDITED (third_party/linux/include/linux/const.h) — vdso/const.h inlined */
#ifndef _LINUX_CONST_H
#define _LINUX_CONST_H

/* shim: drop the vdso dependency; provide the same macros */
#include <linux/types.h>

/* Preserve suffixes on constants used by unchanged Linux size tables. */
#define __AC(x, suffix) (x ## suffix)
#define _AC(x, suffix) __AC(x, suffix)

#define UL(x)		(_UL(x))
#define ULL(x)		(_ULL(x))

#define const_true(x)		(bool)(x)
#define const_false(x)		(!(bool)(x))

#define type_max(type)		((type)~(type)0)

#endif /* _LINUX_CONST_H */
