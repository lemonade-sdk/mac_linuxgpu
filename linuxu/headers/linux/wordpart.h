/* linuxu: AS-IS (third_party/linux/include/linux/wordpart.h) */
#ifndef _LINUX_WORDPART_H
#define _LINUX_WORDPART_H

#include <linux/types.h>
#include <linux/swab.h>

#if defined(__BIG_ENDIAN_BITFIELD)
#define WORDPART_ORDER_BIG
#else
#define WORDPART_ORDER_LITTLE
#endif

#define u32lower32(w)		((u32)(w))
#define u32upper32(w)		((u32)((w) >> 32))

#endif /* _LINUX_WORDPART_H */
