/* test_vram.c — VRAM allocator (REAL): 16KB granularity, bump
 * allocator, pfn<->ptr mapping, size from env. */
#include <stdio.h>
#include <stdlib.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

extern void *linuxu_vram_alloc(size_t size);
extern void linuxu_vram_free(void *ptr);
extern size_t linuxu_vram_size(void);
extern size_t linuxu_vram_used(void);
extern void *vram_pfn_to_ptr(uint32_t pfn);
extern uint32_t vram_ptr_to_pfn(const void *ptr);

int main(void)
{
	/* default env = 16 MB */
	size_t total = linuxu_vram_size();
	EXPECT(total == 16 * 1024 * 1024);

	/* 16KB granularity: odd size rounds up */
	void *a = linuxu_vram_alloc(1);
	EXPECT(a != NULL);
	EXPECT(linuxu_vram_used() == 16 * 1024);

	void *b = linuxu_vram_alloc(1000);
	EXPECT(b != NULL);
	EXPECT(linuxu_vram_used() == 32 * 1024);
	EXPECT(b == (char *)a + 16 * 1024);

	/* pfn mapping round trip at 16KB pages */
	uint32_t pfn = vram_ptr_to_pfn(b);
	EXPECT(vram_pfn_to_ptr(pfn) == b);
	/* pfn 0 is the buffer start */
	EXPECT(vram_pfn_to_ptr(0) == a);

	/* out-of-bounds ptr → sentinel */
	EXPECT(vram_ptr_to_pfn((const void *)0xdeadbeef) == ~0U);

	/* exhaustion: allocate past the end */
	size_t remaining = (total - linuxu_vram_used()) / (16 * 1024);
	size_t big = remaining * 16 * 1024 + 16 * 1024; /* one page over */
	void *oob = linuxu_vram_alloc(big);
	EXPECT(oob == NULL);
	/* and exactly-fits succeeds */
	void *exact = linuxu_vram_alloc(remaining * 16 * 1024);
	EXPECT(exact != NULL);
	EXPECT(linuxu_vram_used() == total);

	linuxu_vram_free(a);
	linuxu_vram_free(b);
	linuxu_vram_free(exact);
	/* NOTE: bump-only allocator: used() does not decrease (TODO) */
	fprintf(stderr, "PASS test_vram\n");
	return 0;
}
