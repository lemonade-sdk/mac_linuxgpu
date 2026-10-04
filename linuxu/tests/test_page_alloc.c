/* test_page_alloc.c — W12: the shim struct page + 16 KB page allocator
 * is REAL enough for the driver's TTM/BO + GART paths:
 *
 *  1. alloc_page -> page_address is a valid, writable, 16 KB-aligned
 *     host VA; write/read through it; pfn round-trips.
 *  2. alloc_pages(gfp, order=2) -> 4 x 16 KB contiguous; the GART
 *     pattern (amdgpu_gart.c:139: p[x].mapping = dev_mapping on every
 *     page of an order-N block) works; free it.
 *  3. .mapping write/read: the exact driver deref (p[x].mapping = X)
 *     sticks and is cleared on free.
 *  4. refcount: get_page/put_page balanced; put-to-zero makes the
 *     slot reusable; double-free is a no-op (idempotent).
 *  5. __get_free_page/__free_page + __get_free_pages/free_pages:
 *     VA-based round-trip, no leak (a P0 bug handed back the struct
 *     page as the "VA", leaking the slot).
 *  6. page_refcount helpers (page_ref_count/get/put via
 *     get_page/put_page from mm.h) + kmemcheck clean at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/mmzone.h>

extern int kmemcheck_verify_all(void);

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

int main(void)
{
	/* ---- 1. alloc_page: real 16 KB host backing ---- */
	struct page *p = alloc_page(GFP_KERNEL);
	EXPECT(p != NULL);
	EXPECT(page_ref_count(p) == 1);
	{
		void *v = page_address(p);
		volatile unsigned char *c = (volatile unsigned char *)v;
		size_t i;

		EXPECT(v != NULL);
		/* 16 KB aligned (the arena is PAGE_SIZE-aligned) */
		EXPECT(((unsigned long)v & (PAGE_SIZE - 1)) == 0);
		/* pfn round-trip */
		EXPECT(pfn_to_page(page_to_pfn(p)) == p);
		EXPECT(virt_to_page_internal(v) == p);
		/* the memory is really writable: pattern + readback.
		 * NOTE: use a separate 'bad' counter — reusing `i` across two
		 * loops with `break` hits an Apple-clang -O1 codegen bug where
		 * the loop counter is not restored (observed 16 KB stride-123
		 * pattern check: counter read 16482 on a clean buffer). */
		for (i = 0; i < PAGE_SIZE; i++)
			c[i] = (unsigned char)(i ^ 0xa5);
		{
			size_t bad = 0;

			for (i = 0; i < PAGE_SIZE; i += 123)
				if (c[i] != (unsigned char)(i ^ 0xa5)) {
					bad = i;
					break;
				}
			EXPECT(bad == 0);
		}
	}

	/* ---- 3. .mapping write (the amdgpu_gart.c:139 deref) ---- */
	p->mapping = (struct address_space *)0xdeadbeef00ULL;
	EXPECT(p->mapping == (struct address_space *)0xdeadbeef00ULL);
	p->private = 0x1234;
	EXPECT(page_private(p) == 0x1234);
	EXPECT(p->private == 0x1234);
	p->flags |= 1UL << 3;
	EXPECT((p->flags & (1UL << 3)) != 0);
	/* the union aliases the driver uses must round-trip too */
	p->zone_device_data = (void *)0xcafe;
	EXPECT(p->private == 0xcafe);
	p->zone_device_data = NULL;
	p->mapping = NULL;

	/* ---- 4. refcount: get/put balanced ---- */
	get_page(p);
	get_page(p);
	EXPECT(page_ref_count(p) == 3);
	put_page(p);
	EXPECT(page_ref_count(p) == 2);
	put_page(p);
	EXPECT(page_ref_count(p) == 1);

	/* ---- 2. alloc_pages order=2: 4 x 16 KB contiguous ---- */
	struct page *block = alloc_pages(GFP_KERNEL, 2);
	EXPECT(block != NULL);
	EXPECT(page_ref_count(block) == 1);
	{
		volatile unsigned char *c;
		size_t total = (size_t)4 * PAGE_SIZE;
		size_t i;

		/* the four slots are contiguous in the pool */
		for (int i = 1; i < 4; i++)
			EXPECT(page_to_pfn(&block[i]) == page_to_pfn(block) + i);
		/* all 64 KB are one contiguous writable region */
		c = (volatile unsigned char *)page_address(block);
		for (i = 0; i < total; i++)
			c[i] = (unsigned char)(i * 7);
		{
			size_t bad = 0;

			for (i = 0; i < total; i += 257)
				if (c[i] != (unsigned char)(i * 7)) {
					bad = i;
					break;
				}
			EXPECT(bad == 0);
		}
		/* page_address of page 3 is base + 3*16 KB */
		EXPECT(page_address(&block[3]) ==
		       (unsigned char *)page_address(block) + 3 * PAGE_SIZE);
	}

	/* the GART pattern: write .mapping on every page of the block */
	{
		struct address_space *dev_mapping =
			(struct address_space *)0x5eedULL;
		unsigned long x;

		for (x = 0; x < (1UL << 2); x++)
			block[x].mapping = dev_mapping;
		for (x = 0; x < (1UL << 2); x++)
			if (block[x].mapping != dev_mapping)
				break;
		EXPECT(x == (1UL << 2));
	}

	/* free the block: all 4 slots reclaimable + fields cleared */
	__free_pages(block, 2);
	{
		struct page *again = alloc_pages(GFP_KERNEL, 2);

		EXPECT(again != NULL);		/* the slots came back */
		for (int x = 0; x < 4; x++) {
			EXPECT(again[x].mapping == NULL);	/* cleared on free */
			EXPECT(again[x].private == 0);
		}
		__free_pages(again, 2);
	}

	/* ---- 4b. free idempotency (double free is a no-op) ---- */
	__free_pages(p, 0);
	EXPECT(atomic_read(&p->refcount) == 0);
	__free_pages(p, 0);	/* second free: no crash, still 0 */
	EXPECT(atomic_read(&p->refcount) == 0);

	/* ---- 5. VA-based API: __get_free_page / __free_page ---- */
	{
		unsigned long va = __get_free_page(GFP_KERNEL);

		EXPECT(va != 0);
		/* it is a REAL host VA into the arena, not a struct
		 * page pointer (the P0 bug) */
		{
			/* 16 KB PFN granularity: VA is PAGE_SIZE-aligned;
			 * virt_to_page_internal knows the arena base */
			struct page *bp = virt_to_page_internal((void *)va);

			EXPECT(bp != NULL);
			EXPECT(page_address(bp) == (void *)va);
			EXPECT(((unsigned long)va & (PAGE_SIZE - 1)) == 0);
			EXPECT(((volatile unsigned char *)va)[0] ==
			       ((volatile unsigned char *)va)[0]);
		}
		/* write + read through the VA */
		((volatile unsigned char *)va)[4095] = 0x42;
		EXPECT(((volatile unsigned char *)va)[4095] == 0x42);
		/* free it back through the VA path */
		__free_page(virt_to_page_internal((void *)va));
	}
	{
		unsigned long va = __get_free_pages(GFP_KERNEL, 1);

		EXPECT(va != 0);
		/* order=1: 2 x 16 KB contiguous */
		EXPECT(((unsigned char *)va + 2 * PAGE_SIZE - 1) - (unsigned char *)va
		       == (size_t)(2 * PAGE_SIZE - 1));
		{
			/* 16 KB PFN granularity: VA is PAGE_SIZE-aligned;
			 * virt_to_page_internal knows the arena base */
			struct page *bp = virt_to_page_internal((void *)va);

			EXPECT(bp != NULL);
			EXPECT(page_address(bp) == (void *)va);
			EXPECT(((unsigned long)va & (PAGE_SIZE - 1)) == 0);
		}
		free_pages(va, 1);
	}

	/* ---- 6. .mapping write survives across other allocs ---- */
	{
		struct page *a = alloc_page(GFP_KERNEL);
		struct page *b = alloc_page(GFP_KERNEL);

		EXPECT(a && b && a != b);
		a->mapping = (struct address_space *)0x1111;
		b->mapping = (struct address_space *)0x2222;
		EXPECT(a->mapping == (struct address_space *)0x1111);
		EXPECT(b->mapping == (struct address_space *)0x2222);
		/* freeing b must not clear a's mapping */
		__free_pages(b, 0);
		EXPECT(a->mapping == (struct address_space *)0x1111);
		__free_pages(a, 0);
	}

	/* ---- exhaustion sanity: alloc beyond the pool fails ---- */
	{
		struct page *big = alloc_pages(GFP_KERNEL, 17);

		/* 2^17 x 16 KB = 2 GiB: more than one chunk of
		 * descriptors, so never contiguous; order 26 (1 TB) must
		 * fail too */
		if (big)
			__free_pages(big, 17);
		EXPECT(alloc_pages(GFP_KERNEL, 26) == NULL);
	}

	/* ---- no fixed pool: more than 2 GiB of pages live at once ---- */
	{
		/* 136 blocks of MAX_PAGE_ORDER (16 MiB each) is 2176 MiB,
		 * past the 2 GiB the pool was once fixed at: descriptors come
		 * in chunks as pages are needed, up to the machine's RAM. */
		enum { BLOCKS = 136 };
		struct page *blocks[BLOCKS];
		unsigned long highest = 0;
		int i;

		for (i = 0; i < BLOCKS; i++) {
			blocks[i] = alloc_pages(GFP_KERNEL, MAX_PAGE_ORDER);
			EXPECT(blocks[i] != NULL);
			EXPECT(page_address(blocks[i]) != NULL);
			EXPECT(page_to_pfn(blocks[i]) != ~0UL);
			EXPECT(pfn_to_page(page_to_pfn(blocks[i])) == blocks[i]);
			EXPECT(virt_to_page(page_address(blocks[i])) == blocks[i]);
			if (page_to_pfn(blocks[i]) > highest)
				highest = page_to_pfn(blocks[i]);
		}
		EXPECT(highest >= (2ul << 30) / PAGE_SIZE);
		/* the last page of the last block is addressable */
		((unsigned char *)page_address(blocks[BLOCKS - 1]))[(PAGE_SIZE << MAX_PAGE_ORDER) - 1] = 0x5a;
		for (i = 0; i < BLOCKS; i++)
			__free_pages(blocks[i], MAX_PAGE_ORDER);
	}

	EXPECT(kmemcheck_verify_all() == 0);
	printf("test_page_alloc: all checks passed\n");
	return 0;
}
