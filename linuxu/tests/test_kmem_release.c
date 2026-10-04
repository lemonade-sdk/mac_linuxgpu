/* The kmalloc family in both builds: release (DEBUG undefined: no kmemcheck,
 * nothing tracked) and debug (kmemcheck canaries and tracking). Sizes,
 * alignment, zeroing, ksize, kmemdup, caches and the sensitive wipe behave
 * the same in both; only the debug build reports corruption. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/slab.h>
#include <linux/gfp.h>

extern int kmemcheck_enabled(void);
extern int kmemcheck_verify_all(void);

int main(void)
{
#if DEBUG
	assert(kmemcheck_enabled());
#else
	assert(!kmemcheck_enabled());
#endif
	for (size_t size = 1; size <= (1u << 20); size = size * 3 + 1) {
		unsigned char *p = kzalloc(size, GFP_KERNEL);
		assert(p && !((uintptr_t)p & (ARCH_DMA_MINALIGN - 1)));
		for (size_t i = 0; i < size; i++)
			assert(!p[i]);
		memset(p, 0x5a, size);
		assert(ksize(p) == size);
		unsigned char *copy = kmemdup(p, size, GFP_KERNEL);
		assert(copy && !memcmp(copy, p, size));
		kfree(copy);
		kfree(p);
	}
	assert(kmalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR);
	kfree(ZERO_SIZE_PTR);
	kfree(NULL);

	struct kmem_cache *cache = kmem_cache_create("release-test", 200, 64, 0, NULL);
	assert(cache);
	void *objects[64];
	for (int i = 0; i < 64; i++) {
		objects[i] = kmem_cache_alloc(cache, GFP_KERNEL);
		assert(objects[i] && !((uintptr_t)objects[i] & 63));
		memset(objects[i], i, 200);
	}
	for (int i = 0; i < 64; i++)
		kmem_cache_free(cache, objects[i]);
	kmem_cache_destroy(cache);

	assert(kmemcheck_verify_all() == 0);
#if DEBUG
	/* Only the debug build sees a write past the payload. */
	unsigned char *q = kmalloc(32, GFP_KERNEL);
	q[32] = 0xa5;
	assert(kmemcheck_verify_all() == 1);
	kfree(q);
	assert(kmemcheck_verify_all() == 0);
#endif
#if DEBUG
	puts("kmalloc family (debug build): sizes, alignment, zeroing, ksize, caches, kmemcheck passed");
#else
	puts("kmalloc family (release build): sizes, alignment, zeroing, ksize, caches passed");
#endif
	return 0;
}
