/* Linux empty-allocation ownership using the production slab allocator. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#ifdef TEST_DRIVERKIT_HEAP
#include "dext_heap_backend.h"
#endif

extern size_t kmemcheck_live_bytes(void);
struct kmemcheck_hdr;
extern int kmemcheck_untrack(struct kmemcheck_hdr *hdr);
static const unsigned char *sensitive_pointer;
static size_t sensitive_size;

/* Inspect bytes immediately before the real canary check and heap release. */
int zero_test_untrack(struct kmemcheck_hdr *hdr)
{
	if (sensitive_pointer) {
		for (size_t i = 0; i < sensitive_size; i++)
			assert(sensitive_pointer[i] == 0);
		sensitive_pointer = NULL;
	}
	return kmemcheck_untrack(hdr);
}

static void empty_allocations(void)
{
	struct device dev = {0};
	const size_t baseline = kmemcheck_live_bytes();
	assert(ZERO_SIZE_PTR == (void *)16 && ZERO_SIZE_PTR != NULL);
	assert(!IS_ERR(ZERO_SIZE_PTR) && ZERO_OR_NULL_PTR(NULL));
	assert(!ZERO_OR_NULL_PTR((void *)17));
	assert(kmalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR);
	assert(kmalloc(0, __GFP_ZERO) == ZERO_SIZE_PTR);
	assert(kzalloc(0, GFP_KERNEL) == ZERO_SIZE_PTR);
	assert(kmalloc_array(0, SIZE_MAX, 0) == ZERO_SIZE_PTR);
	assert(kmalloc_array(SIZE_MAX, 0, 0) == ZERO_SIZE_PTR);
	assert(kcalloc(0, SIZE_MAX, 0) == ZERO_SIZE_PTR);
	assert(kcalloc(SIZE_MAX, 0, 0) == ZERO_SIZE_PTR);
	assert(kvmalloc(0, 0) == ZERO_SIZE_PTR);
	assert(kvzalloc(0, 0) == ZERO_SIZE_PTR);
	assert(kvmalloc_array(SIZE_MAX, 0, 0) == ZERO_SIZE_PTR);
	assert(kvzalloc_array(0, SIZE_MAX, 0) == ZERO_SIZE_PTR);
	assert(kvcalloc(0, SIZE_MAX, 0) == ZERO_SIZE_PTR);
	assert(kmalloc_objs(unsigned int, 0) == ZERO_SIZE_PTR);
	assert(kzalloc_objs(unsigned int, 0) == ZERO_SIZE_PTR);
	assert(kvmalloc_objs(unsigned int, 0) == ZERO_SIZE_PTR);
	assert(kvzalloc_objs(unsigned int, 0) == ZERO_SIZE_PTR);
	assert(kmemdup(NULL, 0, 0) == ZERO_SIZE_PTR);
	assert(kmemdup("", 0, 0) == ZERO_SIZE_PTR);
	assert(memdup_user(NULL, 0) == ZERO_SIZE_PTR);
	assert(vmemdup_user(NULL, 0) == ZERO_SIZE_PTR);
	assert(krealloc(NULL, 0, 0) == ZERO_SIZE_PTR);
	assert(krealloc(ZERO_SIZE_PTR, 0, 0) == ZERO_SIZE_PTR);
	assert(krealloc_array(NULL, SIZE_MAX, 0, 0) == ZERO_SIZE_PTR);
	assert(devm_kmalloc(&dev, 0, 0) == ZERO_SIZE_PTR);
	assert(devm_kzalloc(&dev, 0, 0) == ZERO_SIZE_PTR);
	assert(devm_kcalloc(&dev, 0, SIZE_MAX, 0) == ZERO_SIZE_PTR);
	assert(devm_kcalloc(&dev, SIZE_MAX, 0, 0) == ZERO_SIZE_PTR);
	assert(dev.devres == NULL);
	devm_kfree(&dev, ZERO_SIZE_PTR);
	devres_release_all(&dev);
	assert(dev.devres == NULL);
	/* Every pointer admitted by Linux's low-pointer guard is a no-op. */
	for (uintptr_t p = 0; p <= (uintptr_t)ZERO_SIZE_PTR; p++) {
		assert(ZERO_OR_NULL_PTR((void *)p));
		assert(ksize((void *)p) == 0);
		kfree((void *)p);
		kvfree((void *)p);
		kfree_const((void *)p);
		kfree_sensitive((void *)p);
		kvfree_sensitive((void *)p, SIZE_MAX);
	}
	assert(kmemcheck_live_bytes() == baseline);
}

static void resize_and_sensitive_free(void)
{
	size_t baseline = kmemcheck_live_bytes();
	unsigned char copied[8];
	memset(copied, 0xa7, sizeof(copied));
	const void *foreign = (const void *)(uintptr_t)0x12345;
	/* Linux access_ok only bounds the range by the user address-space
	 * limit; the copy itself faults because no mm maps the address. */
	assert(access_ok(foreign, sizeof(copied)));
	assert(!access_ok((const void *)(TASK_SIZE_MAX - 4), sizeof(copied)));
	assert(!access_ok((const void *)~0UL, 1));
	assert(clear_user((void *)foreign, sizeof(copied)) == sizeof(copied));
	assert(copy_to_user((void *)foreign, copied, sizeof(copied)) == sizeof(copied));
	assert(copy_from_user(copied, foreign, sizeof(copied)) == sizeof(copied));
	for (size_t i = 0; i < sizeof(copied); i++) assert(!copied[i]);
	unsigned int value = UINT_MAX;
	assert(get_user(value, (const unsigned int *)foreign) == -EFAULT && !value);
	assert(put_user(42, (unsigned int *)foreign) == -EFAULT);
	void *pointer = (void *)foreign;
	assert(get_user(pointer, (void *const *)foreign) == -EFAULT && !pointer);
	assert(PTR_ERR(memdup_user(foreign, 8)) == -EFAULT);
	assert(PTR_ERR(vmemdup_user(foreign, 8)) == -EFAULT);
	assert(PTR_ERR(memdup_user_nul(foreign, 8)) == -EFAULT);
	assert(copy_from_user(NULL, foreign, 0) == 0 && copy_to_user(NULL, NULL, 0) == 0);
	assert(clear_user(NULL, 0) == 0);
	unsigned char *p = krealloc(ZERO_SIZE_PTR, 7, __GFP_ZERO);
	assert(p && !ZERO_OR_NULL_PTR(p) && ksize(p) == 7);
	for (size_t i = 0; i < 7; i++) assert(p[i] == 0);
	memset(p, 0x5b, 7);
	unsigned char *grown = krealloc(p, 61, __GFP_ZERO);
	assert(grown && ksize(grown) == 61);
	for (size_t i = 0; i < 7; i++) assert(grown[i] == 0x5b);
	for (size_t i = 7; i < 61; i++) assert(grown[i] == 0);
	assert(krealloc_array(grown, SIZE_MAX, 2, 0) == NULL);
	assert(ksize(grown) == 61 && grown[0] == 0x5b);
	assert(krealloc(grown, 0, 0) == ZERO_SIZE_PTR);
	assert(kmalloc_array(SIZE_MAX, 2, 0) == NULL);
	assert(kcalloc(SIZE_MAX, 2, 0) == NULL);
	assert(kvmalloc_array(SIZE_MAX, 2, 0) == NULL);
	assert(kvzalloc_array(SIZE_MAX, 2, 0) == NULL);
	assert(kmalloc(SIZE_MAX, 0) == NULL);
	assert(PTR_ERR(memdup_user(NULL, 1)) == -EFAULT);
	char *empty_string = memdup_user_nul(NULL, 0);
	assert(!IS_ERR(empty_string) && empty_string[0] == '\0');
	kfree(empty_string);
	const char embedded_nul[] = {'a', '\0', 'b'};
	char *copy = memdup_user_nul(embedded_nul, sizeof(embedded_nul));
	assert(PTR_ERR(copy) == -EFAULT);
	for (int virtual = 0; virtual < 2; virtual++) {
		p = virtual ? kvmalloc(73, 0) : kmalloc(73, 0);
		assert(p && !ZERO_OR_NULL_PTR(p));
		memset(p, 0xa5, 73);
		sensitive_pointer = p;
		sensitive_size = 73;
		if (virtual) kvfree_sensitive(p, 73);
		else kfree_sensitive(p);
		assert(sensitive_pointer == NULL);
	}
	assert(kmemcheck_live_bytes() == baseline);
}

int main(void)
{
	empty_allocations();
	resize_and_sensitive_free();
#ifdef TEST_DRIVERKIT_HEAP
	assert(dext_heap_test_live_allocations() == 0);
	unsigned char *p = kmalloc(17, 0);
	assert(p);
	memset(p, 0x6c, 17);
	dext_heap_test_fail_after(0);
	/* Empty allocation must succeed even if every heap request fails. */
	empty_allocations();
	assert(kmalloc(1, 0) == NULL);
	assert(krealloc(ZERO_SIZE_PTR, 1, 0) == NULL);
	assert(krealloc(p, 71, 0) == NULL);
	assert(ksize(p) == 17 && p[0] == 0x6c);
	assert(krealloc(p, 0, 0) == ZERO_SIZE_PTR);
	dext_heap_test_fail_after(-1);
	assert(dext_heap_test_live_allocations() == 0);
	assert(dext_heap_test_live_bytes() == 0);
#endif
	assert(kmemcheck_live_bytes() == 0);
	puts("Linux zero-size allocation, resize, managed lifetime, and sensitive free passed");
	return 0;
}
