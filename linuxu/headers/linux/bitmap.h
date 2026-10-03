/* linuxu: EDITED (third_party/linux/include/linux/bitmap.h) - upstream bitmap
 * API kept; includes reduced (bitmap-str.h/cleanup.h/find.h dropped -
 * their helpers are provided by the shim's string.h/kernel.h),
 * DEFINE_FREE dropped, numactl variants dropped. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BITMAP_H
#define __LINUX_BITMAP_H

#ifndef __ASSEMBLY__

#include <linux/align.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/types.h>

struct device;

extern bool __test_and_set_bit_region(unsigned long *addr, unsigned int start, unsigned int nbits);
extern bool __test_and_clear_bit_region(unsigned long *addr, unsigned int start, unsigned int nbits);
extern unsigned int __find_first_zero_bit_region(const unsigned long *addr, unsigned int start, unsigned int nbits);

/*
 * Allocation and deallocation of bitmap.
 * Provided in lib/bitmap.c to avoid circular dependency.
 */
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_alloc_node(unsigned int nbits, gfp_t flags, int node);
unsigned long *bitmap_zalloc_node(unsigned int nbits, gfp_t flags, int node);
void bitmap_free(const unsigned long *bitmap);

/* Managed variants of the above. */
unsigned long *devm_bitmap_alloc(struct device *dev,
				 unsigned int nbits, gfp_t flags);
unsigned long *devm_bitmap_zalloc(struct device *dev,
				  unsigned int nbits, gfp_t flags);

/*
 * lib/bitmap.c provides these functions:
 */

bool __bitmap_equal(const unsigned long *bitmap1,
		    const unsigned long *bitmap2, unsigned int nbits);
bool __bitmap_or_equal(const unsigned long *src1,
		       const unsigned long *src2,
		       const unsigned long *src3,
		       unsigned int nbits);
void __bitmap_complement(unsigned long *dst, const unsigned long *src,
			 unsigned int nbits);
void __bitmap_shift_right(unsigned long *dst, const unsigned long *src,
			  unsigned int shift, unsigned int nbits);
void __bitmap_shift_left(unsigned long *dst, const unsigned long *src,
			 unsigned int shift, unsigned int nbits);
void bitmap_cut(unsigned long *dst, const unsigned long *src,
		unsigned int first, unsigned int cut, unsigned int nbits);
bool __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
		 const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
		 const unsigned long *bitmap2, unsigned int nbits);
unsigned int __bitmap_weighted_or(unsigned long *dst, const unsigned long *bitmap1,
				  const unsigned long *bitmap2, unsigned int nbits);
unsigned int __bitmap_weighted_xor(unsigned long *dst, const unsigned long *bitmap1,
				   const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
		  const unsigned long *bitmap2, unsigned int nbits);
bool __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
		    const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_replace(unsigned long *dst,
		      const unsigned long *old, const unsigned long *new,
		      const unsigned long *mask, unsigned int nbits);
bool __bitmap_intersects(const unsigned long *bitmap1,
			 const unsigned long *bitmap2, unsigned int nbits);
bool __bitmap_subset(const unsigned long *bitmap1,
		     const unsigned long *bitmap2, unsigned int nbits);
unsigned int __bitmap_weight(const unsigned long *bitmap, unsigned int nbits);
unsigned int __bitmap_weight_and(const unsigned long *bitmap1,
				 const unsigned long *bitmap2, unsigned int nbits);
unsigned int __bitmap_weight_andnot(const unsigned long *bitmap1,
				    const unsigned long *bitmap2, unsigned int nbits);
void __bitmap_set(unsigned long *map, unsigned int start, int len);
void __bitmap_clear(unsigned long *map, unsigned int start, int len);

unsigned long bitmap_find_next_zero_area_off(unsigned long *map,
					     unsigned long size,
					     unsigned long start,
					     unsigned int nr,
					     unsigned long align_mask,
					     unsigned long align_offset);

/**
 * bitmap_find_next_zero_area - find a contiguous aligned zero area
 */
static __always_inline
unsigned long bitmap_find_next_zero_area(unsigned long *map,
					 unsigned long size,
					 unsigned long start,
					 unsigned int nr,
					 unsigned long align_mask)
{
	return bitmap_find_next_zero_area_off(map, size, start, nr,
					      align_mask, 0);
}

void bitmap_remap(unsigned long *dst, const unsigned long *src,
		const unsigned long *old, const unsigned long *new, unsigned int nbits);
int bitmap_bitremap(int oldbit,
		const unsigned long *old, const unsigned long *new, int bits);
void bitmap_onto(unsigned long *dst, const unsigned long *orig,
		const unsigned long *relmap, unsigned int bits);
void bitmap_fold(unsigned long *dst, const unsigned long *orig,
		unsigned int sz, unsigned int nbits);

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

#define bitmap_size(nbits)	((((size_t)(nbits) + BITS_PER_LONG - 1) / BITS_PER_LONG) * sizeof(unsigned long))

static __always_inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = bitmap_size(nbits);

	if (small_const_nbits(nbits))
		*dst = 0;
	else
		memset(dst, 0, len);
}

static __always_inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = bitmap_size(nbits);

	if (small_const_nbits(nbits))
		*dst = ~0UL;
	else
		memset(dst, 0xff, len);
}

static __always_inline
void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	unsigned int len = bitmap_size(nbits);

	if (small_const_nbits(nbits))
		*dst = *src;
	else
		memcpy(dst, src, len);
}

/*
 * Copy bitmap and clear tail bits in last word.
 */
static __always_inline
void bitmap_copy_clear_tail(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	bitmap_copy(dst, src, nbits);
	if (nbits % BITS_PER_LONG)
		dst[nbits / BITS_PER_LONG] &= BITMAP_LAST_WORD_MASK(nbits);
}

static inline void bitmap_copy_and_extend(unsigned long *to,
					  const unsigned long *from,
					  unsigned int count, unsigned int size)
{
	unsigned int copy = BITS_TO_LONGS(count);

	memcpy(to, from, copy * sizeof(long));
	if (count % BITS_PER_LONG)
		to[copy - 1] &= BITMAP_LAST_WORD_MASK(count);
	memset(to + copy, 0, bitmap_size(size) - copy * sizeof(long));
}

/*
 * On 32-bit systems bitmaps are represented as u32 arrays internally. On LE64
 * machines the order of hi and lo parts of numbers match the bitmap structure.
 */
#if BITS_PER_LONG == 64
void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf,
							unsigned int nbits);
void bitmap_to_arr32(u32 *buf, const unsigned long *bitmap,
							unsigned int nbits);
#else
#define bitmap_from_arr32(bitmap, buf, nbits)			\
	bitmap_copy_clear_tail((unsigned long *) (bitmap),	\
			(const unsigned long *) (buf), (nbits))
#define bitmap_to_arr32(buf, bitmap, nbits)			\
	bitmap_copy_clear_tail((unsigned long *) (buf),		\
			(const unsigned long *) (bitmap), (nbits))
#endif

#if BITS_PER_LONG == 32
void bitmap_from_arr64(unsigned long *bitmap, const u64 *buf, unsigned int nbits);
void bitmap_to_arr64(u64 *buf, const unsigned long *bitmap, unsigned int nbits);
#else
#define bitmap_from_arr64(bitmap, buf, nbits)			\
	bitmap_copy_clear_tail((unsigned long *)(bitmap), (const unsigned long *)(buf), (nbits))
#define bitmap_to_arr64(buf, bitmap, nbits)			\
	bitmap_copy_clear_tail((unsigned long *)(buf), (const unsigned long *)(bitmap), (nbits))
#endif

static __always_inline
bool bitmap_and(unsigned long *dst, const unsigned long *src1,
		const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_and(dst, src1, src2, nbits);
}

static __always_inline
void bitmap_or(unsigned long *dst, const unsigned long *src1,
	       const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 | *src2;
	else
		__bitmap_or(dst, src1, src2, nbits);
}

static __always_inline
unsigned int bitmap_weighted_or(unsigned long *dst, const unsigned long *src1,
				const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits)) {
		*dst = *src1 | *src2;
		return hweight_long(*dst & BITMAP_LAST_WORD_MASK(nbits));
	} else {
		return __bitmap_weighted_or(dst, src1, src2, nbits);
	}
}

static __always_inline
unsigned int bitmap_weighted_xor(unsigned long *dst, const unsigned long *src1,
				 const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits)) {
		*dst = *src1 ^ *src2;
		return hweight_long(*dst & BITMAP_LAST_WORD_MASK(nbits));
	} else {
		return __bitmap_weighted_xor(dst, src1, src2, nbits);
	}
}

static __always_inline
void bitmap_xor(unsigned long *dst, const unsigned long *src1,
		const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 ^ *src2;
	else
		__bitmap_xor(dst, src1, src2, nbits);
}

static __always_inline
bool bitmap_andnot(unsigned long *dst, const unsigned long *src1,
		   const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & ~(*src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_andnot(dst, src1, src2, nbits);
}

static __always_inline
void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = ~(*src);
	else
		__bitmap_complement(dst, src, nbits);
}

#ifdef __LITTLE_ENDIAN
#define BITMAP_MEM_ALIGNMENT 8
#else
#define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif
#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)

static __always_inline
bool bitmap_equal(const unsigned long *src1, const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return !((*src1 ^ *src2) & BITMAP_LAST_WORD_MASK(nbits));
	if (__builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
	    IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		return !memcmp(src1, src2, nbits / 8);
	return __bitmap_equal(src1, src2, nbits);
}

static __always_inline
bool bitmap_or_equal(const unsigned long *src1, const unsigned long *src2,
		     const unsigned long *src3, unsigned int nbits)
{
	if (!small_const_nbits(nbits))
		return __bitmap_or_equal(src1, src2, src3, nbits);

	return !(((*src1 | *src2) ^ *src3) & BITMAP_LAST_WORD_MASK(nbits));
}

static __always_inline
bool bitmap_intersects(const unsigned long *src1, const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ((*src1 & *src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	else
		return __bitmap_intersects(src1, src2, nbits);
}

static __always_inline
bool bitmap_subset(const unsigned long *src1, const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! ((*src1 & ~(*src2)) & BITMAP_LAST_WORD_MASK(nbits));
	else
		return __bitmap_subset(src1, src2, nbits);
}

/* ---- scan helpers (upstream linux/bitmap.h) ---- */
#define for_each_set_bit(bit, mask, size) \
	for ((bit) = find_first_bit((mask), (size)); \
	     (bit) < (size); \
	     (bit) = find_next_bit((mask), (size), (bit) + 1))

#define for_each_clear_bit(bit, mask, size) \
	for ((bit) = find_first_zero_bit((mask), (size)); \
	     (bit) < (size); \
	     (bit) = find_next_zero_bit((mask), (size), (bit) + 1))

#define for_each_or_bit(bit, map, mask, size) \
	for ((bit) = find_next_or_bit((map), (mask), (size), 0); (bit) < (size); \
	     (bit) = find_next_or_bit((map), (mask), (size), (bit) + 1))
#define for_each_andnot_bit(bit, map, mask, size) \
	for ((bit) = find_next_andnot_bit((map), (mask), (size), 0); (bit) < (size); \
	     (bit) = find_next_andnot_bit((map), (mask), (size), (bit) + 1))

static __always_inline
bool bitmap_empty(const unsigned long *src, unsigned nbits)
{
	if (small_const_nbits(nbits))
		return ! (*src & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_bit(src, nbits) == nbits;
}

static __always_inline
bool bitmap_full(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! (~(*src) & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_zero_bit(src, nbits) == nbits;
}

static __always_inline
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight(src, nbits);
}

static __always_inline
unsigned long bitmap_weight_and(const unsigned long *src1,
				const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight_and(src1, src2, nbits);
}

static __always_inline
unsigned long bitmap_weight_andnot(const unsigned long *src1,
				   const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src1 & ~(*src2) & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight_andnot(src1, src2, nbits);
}

/**
 * bitmap_weight_from - Hamming weight for a memory region
 * @bitmap: The base address
 * @start: Start of range in bits (inclusive)
 * @end: End of range in bits (exclusive)
 *
 * Note, the range is not word aligned so this is a bit more costly
 * than other bitmap functions.
 */
static __always_inline
unsigned long bitmap_weight_from(const unsigned long *bitmap,
                                unsigned long start, unsigned long end)
{
	if (start >= end) return end;
	unsigned long count = 0;
	for (unsigned long bit = find_next_bit(bitmap, end, start); bit < end;
	     bit = find_next_bit(bitmap, end, bit + 1)) count++;
	return count;
}

/**
 * bitmap_set - Set a region in a bitmap
 * @map: The address to base the bitmap on
 * @start: bitnum of first bit to set
 * @nbits: number of bits to set
 */
static __always_inline
void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	unsigned int shift = start % BITS_PER_LONG;
	map += start / BITS_PER_LONG;
	while (nbits) {
		unsigned int count = min(nbits, BITS_PER_LONG - shift);
		*map++ |= (~0UL >> (BITS_PER_LONG - count)) << shift;
		nbits -= count;
		shift = 0;
	}
}
static __always_inline
void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	unsigned int shift = start % BITS_PER_LONG;
	map += start / BITS_PER_LONG;
	while (nbits) {
		unsigned int count = min(nbits, BITS_PER_LONG - shift);
		*map++ &= ~((~0UL >> (BITS_PER_LONG - count)) << shift);
		nbits -= count;
		shift = 0;
	}
}
int bitmap_allocate_region(unsigned long *bitmap, unsigned int pos, int order);
void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order);
int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order);

/*
 * bitmap_from_arr32() and friends are already handled above.
 */

/* Fields are at most one machine word and may straddle two bitmap words. */
static __always_inline u64 bitmap_read(const unsigned long *map,
                                       unsigned int start, unsigned int nbits)
{
	if (!nbits || nbits > BITS_PER_LONG) return 0;
	unsigned int shift = start % BITS_PER_LONG, off = start / BITS_PER_LONG;
	unsigned long value = map[off] >> shift;
	if (nbits > BITS_PER_LONG - shift)
		value |= map[off + 1] << (BITS_PER_LONG - shift);
	return value & (~0UL >> (BITS_PER_LONG - nbits));
}
static __always_inline void bitmap_write(unsigned long *map, u64 value,
                                         unsigned int start, unsigned int nbits)
{
	if (!nbits || nbits > BITS_PER_LONG) return;
	unsigned int shift = start % BITS_PER_LONG, off = start / BITS_PER_LONG;
	unsigned long mask = ~0UL >> (BITS_PER_LONG - nbits);
	value &= mask;
	map[off] = (map[off] & ~(mask << shift)) | ((unsigned long)value << shift);
	if (nbits > BITS_PER_LONG - shift) {
		unsigned int rest = nbits - (BITS_PER_LONG - shift);
		mask = ~0UL >> (BITS_PER_LONG - rest);
		map[off + 1] = (map[off + 1] & ~mask) | (value >> (BITS_PER_LONG - shift));
	}
}
static __always_inline u8 bitmap_get_value8(const unsigned long *map, unsigned int start)
{ return bitmap_read(map, start, 8); }
static __always_inline void bitmap_set_value8(unsigned long *map, u8 value, unsigned int start)
{ bitmap_write(map, value, start, 8); }

/*
 * bitmap_to_arr32() - Copy nbits from a bitmap to a u32 array
 */

/*
 * The following parse functions require the string.h helpers provided by
 * the shim's linux/string.h (bitmap_parse, bitmap_parselist, _user
 * variants) - implemented in linuxu/src.
 */
int bitmap_parse(const char *buf, unsigned int buflen,
		 unsigned long *dst, unsigned int nbits);
int bitmap_parselist(const char *buf,
		     unsigned long *dst, unsigned int nbits);
int bitmap_parse_user(const char __user *ubuf, unsigned int ulen,
		      unsigned long *dst, unsigned int nbits);
int bitmap_parselist_user(const char __user *ubuf, unsigned int ulen,
			  unsigned long *dst, unsigned int nbits);

/*
 * bitmap_from_arr64/bitmap_to_arr64 are already handled above.
 */

#endif /* __ASSEMBLY__ */

#endif /* __LINUX_BITMAP_H */

/* ---- bitmap declaration ---- */
#define DECLARE_BITMAP(name, bits) unsigned long name[BITMAP_LONGS(bits)]
#define DEFINE_BITMAP(name, bits) unsigned long name[BITMAP_LONGS(bits)] = { 0 }
#define BITMAP_LONGS(bits) DIV_ROUND_UP(bits, BITS_PER_LONG)
