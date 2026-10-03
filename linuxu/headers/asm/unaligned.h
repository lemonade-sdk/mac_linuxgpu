/* linuxu: SHIM (asm/unaligned.h) — memcpy-based unaligned accessors. */
#ifndef _ASM_UNALIGNED_H
#define _ASM_UNALIGNED_H
#include <linux/types.h>
#include <asm/byteorder.h>
#include <string.h>

#define get_unaligned(p) ({ \
	__typeof__(*(p)) __value; \
	memcpy((void *)&__value, (const void *)(p), sizeof(__value)); \
	__value; \
})
static inline u32 get_unaligned_32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u16 get_unaligned_be16(const void *p) { u16 v; memcpy(&v, p, 2); return be16_to_cpu(v); }
static inline u32 get_unaligned_be32(const void *p) { u32 v; memcpy(&v, p, 4); return be32_to_cpu(v); }
static inline u64 get_unaligned_be64(const void *p) { u64 v; memcpy(&v, p, 8); return be64_to_cpu(v); }
#define put_unaligned(v, p) do { \
	__typeof__(p) __pointer = (p); \
	__typeof__(*__pointer) __value = (v); \
	memcpy((void *)__pointer, (const void *)&__value, sizeof(__value)); \
} while (0)
static inline void put_unaligned32(u32 v, void *p) { memcpy(p, &v, 4); }
static inline void put_unaligned64(u64 v, void *p) { memcpy(p, &v, 8); }
static inline void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static inline void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
static inline void put_unaligned_le64(u64 v, void *p) { memcpy(p, &v, 8); }
static inline void put_unaligned_be16(u16 v, void *p) { v = cpu_to_be16(v); memcpy(p, &v, 2); }
static inline void put_unaligned_be32(u32 v, void *p) { v = cpu_to_be32(v); memcpy(p, &v, 4); }
static inline void put_unaligned_be64(u64 v, void *p) { v = cpu_to_be64(v); memcpy(p, &v, 8); }

#endif
