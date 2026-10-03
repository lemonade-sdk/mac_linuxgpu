/* linuxu: SHIM (third_party/linux/include/linux/hash.h; simplified FNV-1a stand-ins) */
#ifndef __LINUX_HASH_H
#define __LINUX_HASH_H

#include <linux/types.h>
#include <linux/compiler.h>

#ifndef min_t
#define min_t(type, x, y) \
	(({ type __x = (x); type __y = (y); __x < __y ? __x : __y; }))
#endif

static inline u32 hash_64(u64 val, u32 bits)
{
	return (u32)((val * 0x9E3779B97F4A7C15ULL) >> (64 - bits));
}

static __always_inline u32 jhash2(const void *v, u32 r, u32 initval)
{
	/* shim: simple FNV-1a over the data (not the real jhash) */
	const u8 *p = v;
	u32 h = initval ? initval : 0x811c9dc5u;
	u32 i;
	for (i = 0; i < r; i++) {
		h ^= p[i];
		h *= 0x01000193;
	}
	return h;
}
static __always_inline u32 jhash(const void *k, u32 length, u32 initval)
{
	return jhash2(k, length, initval);
}
static __always_inline u32 jhash32(const u32 *k, u32 length, u32 initval)
{
	return jhash2(k, length * sizeof(*k), initval);
}
static __always_inline u64 jhash64(const u64 *k, u32 length, u32 initval)
{
	return (u64)jhash2(k, length * sizeof(*k), initval);
}
#define jhash8(k, v, seed)	jhash2(k, v, seed)

#define hash_min(a, b)		min_t(u32, a, b)
#define hash_long(val, bits) \
	((1L + (val)) % (1UL << (bits)))
#define hash_ptr(ptr, bits) \
	hash_long((unsigned long)(ptr) >> 4, (bits))
#define hash_32(val, bits) \
	((1u + (u32)(val)) % (1u << (bits)))

static __always_inline unsigned int full_name_hash(const unsigned char *name,
						   int len)
{
	return jhash(name, len, 0);
}

#define murmur2_32_gc
#define murmur3_32_gc
#define murmur3_128_gc

#endif /* __LINUX_HASH_H */
