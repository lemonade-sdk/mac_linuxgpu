/* linuxu: SHIM (third_party/linux/include/linux/byteorder/generic.h) */
#ifndef __LINUX_BYTEORDER_GENERIC_H
#define __LINUX_BYTEORDER_GENERIC_H
#include <linux/types.h>
#include <stdint.h>
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define cpu_to_le64(x) ((u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define cpu_to_be64(x) __builtin_bswap64(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be32_to_cpu(x) __builtin_bswap32(x)
#define be64_to_cpu(x) __builtin_bswap64(x)
#endif
