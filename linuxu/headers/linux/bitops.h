/* linuxu: EDITED (third_party/linux/include/linux/bitops.h +
 * asm-generic/bitops/generic-non-atomic.h + builtin-ffs/fls)
 *
 * 64-bit host; BITS_PER_LONG=64. ffs/fls/hweight via Clang builtins;
 * atomic bit operations synchronize concurrent userspace queues.
 */
#ifndef _LINUX_BITOPS_H
#define _LINUX_BITOPS_H

#include <linux/types.h>
#include <linux/bits.h>

/* ---- bit indices: __ffs/__fls are zero-based; ffs/fls are one-based. ---- */
#define __ffs(x) __builtin_ctzl((unsigned long)(x))
#define __ffs64(x) __builtin_ctzll((unsigned long long)(x))
/* First zero bit; undefined when no zero bit exists, as upstream. */
#define ffz(x) __ffs(~(unsigned long)(x))
#define __fls(x) (BITS_PER_LONG - 1 - __builtin_clzl((unsigned long)(x)))
#define __fls64(x) (63 - __builtin_clzll((unsigned long long)(x)))
static __always_inline int generic_ffs(int x)
{ return x ? __builtin_ctz((unsigned int)x) + 1 : 0; }
static __always_inline int constant_ffs(int x) { return generic_ffs(x); }
static __always_inline int generic_fls(int x)
{ return x ? 32 - __builtin_clz((unsigned int)x) : 0; }
static __always_inline int constant_fls(int x) { return generic_fls(x); }
#define ffs(x) generic_ffs(x)
#define fls(x) generic_fls(x)
static __always_inline int fls_long(unsigned long x)
{ return x ? BITS_PER_LONG - __builtin_clzl(x) : 0; }
static __always_inline int linuxu_fls64(unsigned long long x)
{ return x ? 64 - __builtin_clzll(x) : 0; }
static __always_inline int linuxu_ffs64(unsigned long long x)
{ return x ? __builtin_ctzll(x) + 1 : 0; }
#define fls64(x) linuxu_fls64(x)
#define ffs64(x) linuxu_ffs64(x)

#define hweight8(w) __builtin_popcount((u8)(w))
#define hweight16(w) __builtin_popcount((u16)(w))
#define hweight32(w) __builtin_popcount((u32)(w))
#define hweight64(w) __builtin_popcountll((u64)(w))
#define hweight_long(w) __builtin_popcountl((unsigned long)(w))
#define hweight_longlong(w) hweight64(w)
#define popcount(w) hweight32(w)
#define popcount64(w) hweight64(w)
#define popcount_long(w) hweight_long(w)

/* Multiple DriverKit queues access CPU bitmaps. Index into the selected
 * word, preserve the old high bit without narrowing, and order RMW results. */
static __always_inline void set_bit(unsigned long nr, volatile unsigned long *addr)
{ __atomic_fetch_or(addr + BIT_WORD(nr), BIT_MASK(nr), __ATOMIC_RELAXED); }
static __always_inline void clear_bit(unsigned long nr, volatile unsigned long *addr)
{ __atomic_fetch_and(addr + BIT_WORD(nr), ~BIT_MASK(nr), __ATOMIC_RELAXED); }
static __always_inline void change_bit(unsigned long nr, volatile unsigned long *addr)
{ __atomic_fetch_xor(addr + BIT_WORD(nr), BIT_MASK(nr), __ATOMIC_RELAXED); }
static __always_inline int test_bit(unsigned long nr, const volatile unsigned long *addr)
{ return !!(__atomic_load_n(addr + BIT_WORD(nr), __ATOMIC_RELAXED) & BIT_MASK(nr)); }
static __always_inline int test_bit_acquire(unsigned long nr, const volatile unsigned long *addr)
{ return !!(__atomic_load_n(addr + BIT_WORD(nr), __ATOMIC_ACQUIRE) & BIT_MASK(nr)); }
static __always_inline int test_and_set_bit(unsigned long nr, volatile unsigned long *addr)
{ return !!(__atomic_fetch_or(addr + BIT_WORD(nr), BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)); }
static __always_inline int test_and_clear_bit(unsigned long nr, volatile unsigned long *addr)
{ return !!(__atomic_fetch_and(addr + BIT_WORD(nr), ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)); }
static __always_inline int test_and_change_bit(unsigned long nr, volatile unsigned long *addr)
{ return !!(__atomic_fetch_xor(addr + BIT_WORD(nr), BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)); }
static __always_inline int test_and_set_bit_lock(unsigned long nr, volatile unsigned long *addr)
{ return !!(__atomic_fetch_or(addr + BIT_WORD(nr), BIT_MASK(nr), __ATOMIC_ACQUIRE) & BIT_MASK(nr)); }

/* op: plain, complement, union, intersection, difference. */
static inline unsigned long linuxu_find_bit(const unsigned long *a,
        const unsigned long *b, unsigned long size, unsigned long start, int op)
{
	if (start >= size) return size;
	unsigned long index = start / BITS_PER_LONG;
	unsigned long last = (size - 1) / BITS_PER_LONG;
	for (;;) {
		unsigned long word = __atomic_load_n(a + index, __ATOMIC_RELAXED);
		if (op == 1) word = ~word;
		if (op == 2) word |= __atomic_load_n(b + index, __ATOMIC_RELAXED);
		if (op == 3) word &= __atomic_load_n(b + index, __ATOMIC_RELAXED);
		if (op == 4) word &= ~__atomic_load_n(b + index, __ATOMIC_RELAXED);
		if (index == start / BITS_PER_LONG) word &= ~0UL << (start % BITS_PER_LONG);
		if (word) {
			unsigned long bit = index * BITS_PER_LONG + __ffs(word);
			return bit < size ? bit : size;
		}
		if (index == last) return size;
		index++;
	}
}
static inline unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long start)
{ return linuxu_find_bit(a, NULL, size, start, 0); }
static inline unsigned long find_first_bit(const unsigned long *a, unsigned long size)
{ return find_next_bit(a, size, 0); }
static inline unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long start)
{ return linuxu_find_bit(a, NULL, size, start, 1); }
static inline unsigned long find_first_zero_bit(const unsigned long *a, unsigned long size)
{ return find_next_zero_bit(a, size, 0); }
static inline unsigned long find_next_or_bit(const unsigned long *a, const unsigned long *b, unsigned long size, unsigned long start)
{ return linuxu_find_bit(a, b, size, start, 2); }
static inline unsigned long find_next_and_bit(const unsigned long *a, const unsigned long *b, unsigned long size, unsigned long start)
{ return linuxu_find_bit(a, b, size, start, 3); }
static inline unsigned long find_next_andnot_bit(const unsigned long *a, const unsigned long *b, unsigned long size, unsigned long start)
{ return linuxu_find_bit(a, b, size, start, 4); }
static inline unsigned long find_last_bit(const unsigned long *a, unsigned long size)
{
	if (!size) return size;
	unsigned long index = (size - 1) / BITS_PER_LONG;
	unsigned long word = __atomic_load_n(a + index, __ATOMIC_RELAXED);
	if (size % BITS_PER_LONG) word &= ~0UL >> (BITS_PER_LONG - size % BITS_PER_LONG);
	for (;;) {
		if (word) return index * BITS_PER_LONG + __fls(word);
		if (!index) return size;
		word = __atomic_load_n(a + --index, __ATOMIC_RELAXED);
	}
}

#define for_each_set_bit(pos, mask, size) \
	for ((pos) = find_first_bit((mask), (size)); (pos) < (size); \
	     (pos) = find_next_bit((mask), (size), (pos) + 1))
#define for_each_clear_bit(pos, mask, size) \
	for ((pos) = find_first_zero_bit((mask), (size)); (pos) < (size); \
	     (pos) = find_next_zero_bit((mask), (size), (pos) + 1))
#define for_each_set_bit_from(pos, mask, size) \
	for ((pos) = find_next_bit((mask), (size), (pos)); (pos) < (size); \
	     (pos) = find_next_bit((mask), (size), (pos) + 1))
#define for_each_clear_bit_from(pos, mask, size) \
	for ((pos) = find_next_zero_bit((mask), (size), (pos)); (pos) < (size); \
	     (pos) = find_next_zero_bit((mask), (size), (pos) + 1))
#define for_each_set_bit_mask(pos, mask, bitmap) for_each_set_bit(pos, bitmap, BITS_PER_LONG)
#define for_each_clear_bit_mask(pos, mask, bitmap) for_each_clear_bit(pos, bitmap, BITS_PER_LONG)

/* ---- min/max/clamp (kernel arg order: min(type, a, b)) ---- */
#define min(a, b) \
	({ typeof(a) _mina1 = (a); typeof(b) _minb1 = (b); \
	    _mina1 < _minb1 ? _mina1 : _minb1; })
#define max(a, b) \
	({ typeof(a) _maxa1 = (a); typeof(b) _maxb1 = (b); \
	    _maxa1 > _maxb1 ? _maxa1 : _maxb1; })
#define min_t(type, a, b) \
	({ type _mina2 = (a); type _minb2 = (b); _mina2 < _minb2 ? _mina2 : _minb2; })
#define max_t(type, a, b) \
	({ type _maxa2 = (a); type _maxb2 = (b); _maxa2 > _maxb2 ? _maxa2 : _maxb2; })
/* N-ary min/max (vendor 2026 minmax.h) */
#define min3(x, y, z) \
	({ typeof(x) _a = (x); typeof(y) _b = (y); typeof(z) _c = (z); \
	    min(min(_a, _b), _c); })
#define max3(x, y, z) \
	({ typeof(x) _a = (x); typeof(y) _b = (y); typeof(z) _c = (z); \
	    max(max(_a, _b), _c); })
#define min4(x, y, z, w) min(min3(x, y, z), w)
#define max4(x, y, z, w) max(max3(x, y, z), w)
#define min4_t(type, x, y, z, w) min4(x, y, z, w)
#define max4_t(type, x, y, z, w) max4(x, y, z, w)
#define MAX_T(type, a, b) max_t(type, a, b)
#define MIN_T(type, a, b) min_t(type, a, b)
#define clamp(val, lo, hi) \
	({ typeof(val) _val = (val); typeof(lo) _lo = (lo); typeof(hi) _hi = (hi); \
	    _val < _lo ? _lo : (_val > _hi ? _hi : _val); })
#define clamp_val clamp
#define clamp_t(type, val, lo, hi) clamp(val, lo, hi)

/* ---- bit field helpers ---- */
#define bit_hweight8(w)		hweight8(w)

/* ---- set/clear ranges ---- */
static inline void set_bits(unsigned long start, unsigned long end,
			    unsigned long *addr)
{
	unsigned long i;
	for (i = start; i <= end; i++)
		set_bit(i, addr);
}

static inline void clear_bits(unsigned long start, unsigned long end,
			      unsigned long *addr)
{
	unsigned long i;
	for (i = start; i <= end; i++)
		clear_bit(i, addr);
}



#ifndef upper_32_bits
#define upper_32_bits(n)	((u32)(((n) >> 32) & 0xffffffffUL))
#endif
#ifndef lower_32_bits
#define lower_32_bits(n)	((u32)((n) & 0xffffffffUL))
#endif

#ifndef is_power_of_2
#define is_power_of_2(n)	(((n) != 0) && !((n) & ((n) - 1)))
#endif

static __always_inline void clear_bit_unlock(unsigned long nr, volatile unsigned long *addr)
{
	__atomic_fetch_and(addr + BIT_WORD(nr), ~BIT_MASK(nr), __ATOMIC_RELEASE);
}
static __always_inline void test_bit_unlock(unsigned long nr, volatile unsigned long *addr)
{
	(void)nr; (void)addr;
}
static __always_inline void set_bit_lock(unsigned long nr, volatile unsigned long *addr)
{
	set_bit(nr, addr);
}
#endif /* _LINUX_BITOPS_H */
#define __set_bit(nr, addr) set_bit(nr, addr)
#define __clear_bit(nr, addr) clear_bit(nr, addr)
#define __test_and_set_bit(nr, addr) test_and_set_bit(nr, addr)

