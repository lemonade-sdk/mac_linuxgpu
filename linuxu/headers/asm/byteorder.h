/* linuxu: SHIM (asm/byteorder.h — the unconditional <asm/byteorder.h>
 * include in third_party/linux/drivers/gpu/drm/amd/display/dc/os_types.h and dmub_cmd.h).
 * Self-contained: defines cpu_to_le32 & friends as LE-identity.  The
 * upstream arm64 variant just forwards to
 * <linux/byteorder/little_endian.h>, which is not part of the 5-spike
 * closure; to keep this header standalone (and not depend on the
 * linux/ chunk providing that file), the macros live here.
 * NOTE: <linux/types.h> must be included before this header (it is,
 * via <linux/types.h> in every translation unit that reaches this). */
#include <linux/types.h>

#ifndef __ASM_BYTEORDER_H
#define __ASM_BYTEORDER_H

/* Little-endian identity conversions (Apple Silicon is LE). */
static inline u8   __cpu_to_le8(u8 x)   { return x; }
static inline u16  __cpu_to_le16(u16 x) { return x; }
static inline u32  __cpu_to_le32(u32 x) { return x; }
static inline u64  __cpu_to_le64(u64 x) { return x; }

static inline u8   __le8_to_cpu(u8 x)   { return x; }
static inline u16  __le16_to_cpu(u16 x) { return x; }
static inline u32  __le32_to_cpu(u32 x) { return x; }
static inline u64  __le64_to_cpu(u64 x) { return x; }

/* Big-endian conversions (LE host -> BE). */
static inline u16  __cpu_to_be16(u16 x)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap16(x);
#else
	return x;
#endif
}
static inline u32  __cpu_to_be32(u32 x)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap32(x);
#else
	return x;
#endif
}
static inline u64  __cpu_to_be64(u64 x)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(x);
#else
	return x;
#endif
}
static inline u16  __be16_to_cpu(u16 x) { return __cpu_to_be16(x); }
static inline u32  __be32_to_cpu(u32 x) { return __cpu_to_be32(x); }
static inline u64  __be64_to_cpu(u64 x) { return __cpu_to_be64(x); }

#define cpu_to_le8(x)   ((__le8) __cpu_to_le8(x))
#define cpu_to_le16(x)  ((__le16) __cpu_to_le16(x))
#define cpu_to_le32(x)  ((__le32) __cpu_to_le32(x))
#define cpu_to_le64(x)  ((__le64) __cpu_to_le64(x))
#define le8_to_cpu(x)   ((__u8)  __le8_to_cpu((__le8) (x)))
#define le16_to_cpu(x)  ((__u16) __le16_to_cpu((__le16) (x)))
#define le32_to_cpu(x)  ((__u32) __le32_to_cpu((__le32) (x)))
#define le64_to_cpu(x)  ((__u64) __le64_to_cpu((__le64) (x)))

#define cpu_to_be8(x)   ((x))
#define cpu_to_be16(x)  ((__be16) __cpu_to_be16(x))
#define cpu_to_be32(x)  ((__be32) __cpu_to_be32(x))
#define cpu_to_be64(x)  ((__be64) __cpu_to_be64(x))
#define be8_to_cpu(x)   ((x))
#define be16_to_cpu(x)  ((__u16) __be16_to_cpu((__be16) (x)))
#define be32_to_cpu(x)  ((__u32) __be32_to_cpu((__be32) (x)))
#define be64_to_cpu(x)  ((__u64) __be64_to_cpu((__be64) (x)))

#endif	/* __ASM_BYTEORDER_H */
