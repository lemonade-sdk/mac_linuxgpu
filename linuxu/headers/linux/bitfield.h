/* linuxu: SHIM (third_party/linux/include/linux/bitfield.h) */
#ifndef __LINUX_BITFIELD_H
#define __LINUX_BITFIELD_H
#include <linux/types.h>
#include <linux/bits.h>

#define FIELD_GET(mask, reg) ({ \
	__typeof__(mask) __mask = (mask); \
	(__typeof__(mask))(((reg) & __mask) >> __builtin_ctzll((u64)__mask)); \
})
#endif
