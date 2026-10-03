/* CPU bitmap operations used by upstream resource and scheduler bookkeeping. */
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/device.h>

unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)
{ return kmalloc(bitmap_size(nbits), flags); }
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)
{ return kzalloc(bitmap_size(nbits), flags); }
unsigned long *bitmap_alloc_node(unsigned int nbits, gfp_t flags, int node)
{ (void)node; return bitmap_alloc(nbits, flags); }
unsigned long *bitmap_zalloc_node(unsigned int nbits, gfp_t flags, int node)
{ (void)node; return bitmap_zalloc(nbits, flags); }
void bitmap_free(const unsigned long *bitmap) { kfree(bitmap); }
unsigned long *devm_bitmap_alloc(struct device *dev, unsigned int nbits, gfp_t flags)
{ return devm_kmalloc(dev, bitmap_size(nbits), flags); }
unsigned long *devm_bitmap_zalloc(struct device *dev, unsigned int nbits, gfp_t flags)
{ return devm_kzalloc(dev, bitmap_size(nbits), flags); }

static unsigned long tail_mask(unsigned int nbits, size_t index)
{
	return nbits % BITS_PER_LONG && index == nbits / BITS_PER_LONG ?
		BITMAP_LAST_WORD_MASK(nbits) : ~0UL;
}
static unsigned int binary(unsigned long *dst, const unsigned long *a,
                            const unsigned long *b, unsigned int nbits, int op)
{
	unsigned int weight = 0;
	for (size_t i = 0; i < bitmap_size(nbits) / sizeof(long); ++i) {
		unsigned long word = op == 0 ? (a[i] & b[i]) : op == 1 ? (a[i] | b[i]) :
			op == 2 ? (a[i] ^ b[i]) : (a[i] & ~b[i]);
		word &= tail_mask(nbits, i);
		if (dst) dst[i] = word;
		weight += hweight_long(word);
	}
	return weight;
}
bool __bitmap_and(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(d, a, b, n, 0) != 0; }
void __bitmap_or(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ (void)binary(d, a, b, n, 1); }
void __bitmap_xor(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ (void)binary(d, a, b, n, 2); }
bool __bitmap_andnot(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(d, a, b, n, 3) != 0; }
unsigned int __bitmap_weighted_or(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(d, a, b, n, 1); }
unsigned int __bitmap_weighted_xor(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(d, a, b, n, 2); }
unsigned int __bitmap_weight_and(const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(NULL, a, b, n, 0); }
unsigned int __bitmap_weight_andnot(const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(NULL, a, b, n, 3); }
bool __bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(NULL, a, b, n, 0) != 0; }
bool __bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(NULL, a, b, n, 3) == 0; }
bool __bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned n)
{ return binary(NULL, a, b, n, 2) == 0; }
bool __bitmap_or_equal(const unsigned long *a, const unsigned long *b, const unsigned long *c, unsigned n)
{
	for (size_t i = 0; i < bitmap_size(n) / sizeof(long); ++i)
		if (((a[i] | b[i]) ^ c[i]) & tail_mask(n, i)) return false;
	return true;
}
unsigned int __bitmap_weight(const unsigned long *a, unsigned n)
{
	unsigned int count = 0;
	for (size_t i = 0; i < bitmap_size(n) / sizeof(long); ++i)
		count += hweight_long(a[i] & tail_mask(n, i));
	return count;
}
void __bitmap_complement(unsigned long *d, const unsigned long *s, unsigned n)
{
	for (size_t i = 0; i < bitmap_size(n) / sizeof(long); ++i)
		d[i] = ~s[i] & tail_mask(n, i);
}
void __bitmap_replace(unsigned long *d, const unsigned long *old,
                      const unsigned long *new, const unsigned long *mask, unsigned n)
{
	for (size_t i = 0; i < bitmap_size(n) / sizeof(long); ++i)
		d[i] = ((old[i] & ~mask[i]) | (new[i] & mask[i])) & tail_mask(n, i);
}
void __bitmap_set(unsigned long *map, unsigned start, int len)
{ if (len > 0) bitmap_set(map, start, (unsigned)len); }
void __bitmap_clear(unsigned long *map, unsigned start, int len)
{ if (len > 0) bitmap_clear(map, start, (unsigned)len); }
void bitmap_from_arr32(unsigned long *dst, const u32 *src, unsigned nbits)
{
	for (size_t i = 0; i < bitmap_size(nbits) / sizeof(long); ++i) {
		unsigned long word = src[2 * i];
		if ((uint64_t)i * 64 + 32 < nbits) word |= (unsigned long)src[2 * i + 1] << 32;
		dst[i] = word & tail_mask(nbits, i);
	}
}
void bitmap_to_arr32(u32 *dst, const unsigned long *src, unsigned nbits)
{
	for (size_t i = 0; i < ((size_t)nbits + 31) / 32; ++i) {
		u32 word = src[i / 2] >> (32 * (i % 2));
		if (nbits % 32 && i == nbits / 32) word &= ~0U >> (32 - nbits % 32);
		dst[i] = word;
	}
}
unsigned long bitmap_find_next_zero_area_off(unsigned long *map, unsigned long size,
        unsigned long start, unsigned int nr, unsigned long align_mask, unsigned long align_offset)
{
	for (;;) {
		unsigned long index = find_next_zero_bit(map, size, start), aligned, end;
		if (__builtin_add_overflow(index, align_offset, &aligned) ||
		    __builtin_add_overflow(aligned, align_mask, &aligned)) return ~0UL;
		aligned &= ~align_mask;
		if (aligned < align_offset) return ~0UL;
		index = aligned - align_offset;
		if (__builtin_add_overflow(index, (unsigned long)nr, &end)) return ~0UL;
		if (end > size) return end;
		unsigned long occupied = find_next_bit(map, end, index);
		if (occupied == end) return index;
		start = occupied + 1;
	}
}
int bitmap_allocate_region(unsigned long *map, unsigned pos, int order)
{
	if (order < 0 || order >= 31 || (pos & ((1u << order) - 1)) ||
	    pos > UINT_MAX - (1u << order)) return -EINVAL;
	unsigned end = pos + (1u << order);
	if (find_next_bit(map, end, pos) < end) return -EBUSY;
	bitmap_set(map, pos, 1u << order);
	return 0;
}
void bitmap_release_region(unsigned long *map, unsigned pos, int order)
{
	if (order >= 0 && order < 31 && !(pos & ((1u << order) - 1)) &&
	    pos <= UINT_MAX - (1u << order)) bitmap_clear(map, pos, 1u << order);
}
int bitmap_find_free_region(unsigned long *map, unsigned bits, int order)
{
	if (order < 0 || order >= 31) return -EINVAL;
	unsigned count = 1u << order;
	unsigned long pos = bitmap_find_next_zero_area_off(map, bits, 0, count, count - 1, 0);
	if (pos > bits || count > bits - pos || pos > INT_MAX) return -ENOMEM;
	bitmap_set(map, pos, count);
	return (int)pos;
}
