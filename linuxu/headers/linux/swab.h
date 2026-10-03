/* linuxu: EDITED (vendor uapi/linux/swab.h; clang builtins) */
#ifndef __LINUX_SWAB_H
#define __LINUX_SWAB_H

#include <linux/compiler.h>

#define swab16(x)	(__builtin_bswap16(x))
#define swab32(x)	(__builtin_bswap32(x))
#define swab64(x)	(__builtin_bswap64(x))
#define swabp16(x)	(*(__be16 *)(x) = __constant_cpu_to_be16((__be16 *)(x)))
#define swabp32(x)	(*(__be32 *)(x) = __constant_cpu_to_be32((__be32 *)(x)))
#define swabp64(x)	(*(__be64 *)(x) = __constant_cpu_to_be64((__be64 *)(x)))

#define __swab16(x)		(__builtin_constant_p(x) ? \
				(__u16)__constant_swab16(x) : \
				__swab16_notconst(x))
#define __swab32(x)		(__builtin_constant_p(x) ? \
				(__u32)__constant_swab32(x) : \
				__swab32_notconst(x))
#define __swab64(x)		(__builtin_constant_p(x) ? \
				(__u64)__constant_swab64(x) : \
				__swab64_notconst(x))
#define __constant_swab16(x)	((__u16)(__builtin_bswap16(x)))
#define __constant_swab32(x)	((__u32)(__builtin_bswap32(x)))
#define __constant_swab64(x)	((__u64)(__builtin_bswap64(x)))
static __always_inline __u16 __swab16_notconst(__u16 x)
{
	return __builtin_bswap16(x);
}
static __always_inline __u32 __swab32_notconst(__u32 x)
{
	return __builtin_bswap32(x);
}
static __always_inline __u64 __swab64_notconst(__u64 x)
{
	return __builtin_bswap64(x);
}

#endif /* __LINUX_SWAB_H */
