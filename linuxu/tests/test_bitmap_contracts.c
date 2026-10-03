/* Real bitmap primitives: guard words, variable lengths and concurrent bitsets. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <asm/div64.h>

void *kmalloc(size_t n, gfp_t f) { (void)f; return malloc(n); }
void *kzalloc(size_t n, gfp_t f) { (void)f; return calloc(1, n); }
void kfree(const void *p) { free((void *)p); }
static unsigned long concurrent[2];
static void *set_worker(void *opaque)
{
	unsigned long bit = (uintptr_t)opaque;
	for (unsigned i = 0; i < 10000; ++i) {
		assert(!test_and_set_bit(bit, concurrent));
		assert(test_and_set_bit(bit, concurrent));
		assert(test_and_clear_bit(bit, concurrent));
		assert(!test_and_clear_bit(bit, concurrent));
	}
	set_bit(bit, concurrent); return NULL;
}
static void fields(void)
{
	for (unsigned start = 0; start < 128; ++start) {
		for (unsigned width = 0; width <= 64; ++width) {
			unsigned long a[3] = {~0UL, ~0UL, ~0UL};
			unsigned long before[3]; memcpy(before, a, sizeof(a));
			bitmap_write(a, 0x9999999999999999ull, start, width);
			for (unsigned bit = 0; bit < 192; ++bit) {
				bool expected = bit >= start && bit - start < width ?
					!!(0x9999999999999999ull & (1ull << (bit - start))) : true;
				assert(test_bit(bit, a) == expected);
			}
			unsigned long mask = !width ? 0 : ~0UL >> (64 - width);
			assert(bitmap_read(a, start, width) == (0x9999999999999999ull & mask));
			bitmap_write(a, ~0ull, start, width);
			assert(!memcmp(before, a, sizeof(a)));
		}
		unsigned long a[3] = {0};
		bitmap_set_value8(a, 0xab, start);
		assert(bitmap_get_value8(a, start) == 0xab);
	}
}
static void arithmetic(void)
{
	u64 n = 0x100000005ULL; u32 remainder;
	assert(do_div(n, 3) == 0 && n == 0x55555557ULL);
	n = 0x1000000005ULL;
	assert(div_u64(n, 2) == 0x800000002ULL);
	assert(div_u64_rem(n, 2, &remainder) == 0x800000002ULL);
	assert(remainder == 1 && n == 0x1000000005ULL);
	for (unsigned d = 1; d < 40; ++d) for (u64 value = 0; value < 200; ++value) {
		n = value; assert(do_div(n, d) == value % d && n == value / d);
		assert(DIV_ROUND_DOWN_ULL(value, d) == value / d);
		assert(DIV_ROUND_UP_ULL(value, d) == (value + d - 1) / d);
		assert(DIV_ROUND_CLOSEST_ULL(value, d) == (value + d / 2) / d);
	}
	assert(DIV_ROUND_UP_ULL(~0ULL, 2) == (1ULL << 63));
	assert(DIV_ROUND_CLOSEST_ULL(~0ULL, 2) == (1ULL << 63));
	assert(round_up(0x100000001ULL, 4096u) == 0x100001000ULL);
	assert(round_down(0x100000fffULL, 4096u) == 0x100000000ULL);
	for (unsigned bit=0; bit<63; ++bit) {
		assert(ilog2(1ULL << bit) == bit);
		assert(roundup_pow_of_two(1ULL << bit) == (1ULL << bit));
		if (bit) assert(roundup_pow_of_two((1ULL << bit)+1) == (1ULL << (bit+1)));
	}
	for (unsigned value=0;value<256;++value) {
		assert(ALIGN_DOWN(value, 16) == value / 16 * 16);
		assert(ALIGN(value, 16) == (value+15) / 16 * 16);
	}
}
int main(void)
{
	arithmetic();
	assert(bitmap_weight_from(NULL, 64, 64) == 64);
	assert(bitmap_weight_from(NULL, 65, 64) == 64);
	assert(ffs(0) == 0 && fls(0) == 0 && fls64(0) == 0 && fls_long(0) == 0);
	for (unsigned i = 0; i < 32; ++i) {
		volatile unsigned x = 1u << i;
		assert(ffs(x) == i + 1 && fls(x) == i + 1);
		assert(__ffs(x) == i && __fls(x) == i);
	}
	for (unsigned i = 0; i < 64; ++i) {
		assert(fls64(1ull << i) == i + 1 && fls_long(1ul << i) == i + 1);
	}
	assert(hweight8(0x100) == 0 && hweight16(0x10000) == 0);
	for (unsigned start = 0; start < 193; ++start) {
		for (unsigned length = 0; length <= 256 - start; ++length) {
			unsigned long a[6] = {0}; a[4] = 0x1122334455667788ul; a[5] = ~0UL;
			bitmap_set(a, start, length);
			for (unsigned i = 0; i < 256; ++i) assert(test_bit(i, a) == (i >= start && i - start < length));
			assert(a[4] == 0x1122334455667788ul && a[5] == ~0UL);
			assert(bitmap_weight(a, 256) == length);
			bitmap_clear(a, start, length);
			assert(bitmap_empty(a, 256));
		}
	}
	unsigned long a[3] = {0, 1ul << 63, ~0UL}, b[3] = {0};
	assert(find_first_bit(a, 100) == 100 && find_last_bit(a, 100) == 100);
	set_bit(7, a); set_bit(65, a); set_bit(95, a);
	assert(find_last_bit(a, 100) == 95);
	assert(find_next_bit(a, 100, ~0UL) == 100);
	assert(find_first_bit(NULL, 0) == 0 && find_last_bit(NULL, 0) == 0);
	unsigned bit = 8, count = 0;
	for_each_set_bit_from(bit, a, 100) count++;
	assert(count == 2);
	set_bit(65, b); set_bit(99, b);
	count = 0; for_each_or_bit(bit, a, b, 100) count++;
	assert(count == 4);
	count = 0; for_each_andnot_bit(bit, a, b, 100) count++;
	assert(count == 2);
	assert(bitmap_weight_from(a, 8, 100) == 2);
	unsigned long out[3] = {0};
	assert(bitmap_and(out, a, b, 100) && bitmap_weight(out, 100) == 1);
	bitmap_or(out, a, b, 100); assert(bitmap_weight(out, 100) == 4);
	bitmap_complement(out, a, 100); assert(bitmap_weight(out, 100) == 97);
	assert(bitmap_intersects(a, b, 100));
	for (unsigned n = 0; n <= 129; ++n) {
		u32 src[5] = {0x80000001, 0xffffffff, 0xaabbccdd, 0x33334444, ~0u};
		u32 dst[6] = {0}; dst[(n + 31) / 32] = 0xdeadbeef;
		unsigned long words[4] = {0}; words[(n + 63) / 64] = 0x12345678;
		bitmap_from_arr32(words, src, n); bitmap_to_arr32(dst, words, n);
		assert(dst[(n + 31) / 32] == 0xdeadbeef);
		assert(words[(n + 63) / 64] == 0x12345678);
		for (unsigned i = 0; i < n; ++i) assert(((dst[i / 32] >> (i % 32)) & 1) == ((src[i / 32] >> (i % 32)) & 1));
	}
	memset(a, 0, sizeof(a));
	assert(bitmap_allocate_region(a, 64, 5) == 0);
	assert(bitmap_allocate_region(a, 64, 5) == -EBUSY);
	assert(bitmap_find_free_region(a, 128, 6) == 0);
	assert(bitmap_find_free_region(a, 128, 6) == -ENOMEM);
	bitmap_release_region(a, 64, 5);
	assert(bitmap_find_free_region(a, 128, 6) == 64);
	assert(bitmap_find_next_zero_area_off(a, 128, ~0UL, 1, 7, ~0UL) == ~0UL);
	fields();
	pthread_t threads[32];
	for (unsigned i = 0; i < 32; ++i) assert(!pthread_create(&threads[i], NULL, set_worker, (void *)(uintptr_t)(i * 4)));
	for (unsigned i = 0; i < 32; ++i) assert(!pthread_join(threads[i], NULL));
	assert(bitmap_weight(concurrent, 128) == 32);
	unsigned long *allocated = bitmap_zalloc(130, 0);
	assert(allocated && bitmap_empty(allocated, 130));
	set_bit(129, allocated); assert(bitmap_weight(allocated, 130) == 1); bitmap_free(allocated);
	puts("bitmap bounds, field masks, bit indices, region ownership and concurrent RMW passed");
}
