/* test_bo_mmap.c — W5: the GEM/TTM mmap pre-fault path returns a REAL
 * live in-process pointer at mmap time, and a later fault is a no-op
 * "already mapped" (the in-process win: BO mmap == usable pointer).
 *
 * 1. create a shim BO (4 anon backing pages)
 * 2. linuxu_ttm_bo_shim_mmap -> non-NULL in-process pointer
 * 3. write a pattern through it, read it back, verify (it is real RAM)
 * 4. mmap again -> "already mapped", SAME pointer, no double-map
 * 5. fault after mmap -> "already mapped" no-op (counted, no change)
 * 6. teardown: kmemcheck clean (backing pages freed, nothing leaked)
 */
#include <stdio.h>
#include <string.h>

#include <linux/types.h>
#include <linux/dma-mapping.h>

extern int kmemcheck_verify_all(void);

/* the shim BO host primitives (linuxu/src/drm/ttm.c, W5) */
struct linuxu_ttm_bo_shim;
extern struct linuxu_ttm_bo_shim *linuxu_ttm_bo_shim_create(int nr_pages);
extern int linuxu_ttm_bo_shim_mmap(struct linuxu_ttm_bo_shim *bo,
				   void **out_vaddr);
extern int linuxu_ttm_bo_shim_fault(struct linuxu_ttm_bo_shim *bo);
extern void linuxu_ttm_bo_shim_unmap(struct linuxu_ttm_bo_shim *bo);
extern void linuxu_ttm_bo_shim_destroy(struct linuxu_ttm_bo_shim *bo);

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

#define NR_PAGES 4

int main(void)
{
	struct linuxu_ttm_bo_shim *bo;
	void *p1 = NULL, *p2 = NULL;
	int rc;

	/* 1. create the BO: 4 anon backing pages */
	bo = linuxu_ttm_bo_shim_create(NR_PAGES);
	EXPECT(bo != NULL);

	/* fault before mmap: nothing mapped yet */
	EXPECT(linuxu_ttm_bo_shim_fault(bo) == -2);

	/* 2. mmap: must return a live in-process pointer */
	rc = linuxu_ttm_bo_shim_mmap(bo, &p1);
	EXPECT(rc == 0);
	EXPECT(p1 != NULL);

	/* 3. the pointer is real RAM: write a pattern, read it back */
	{
		volatile unsigned char *c = (volatile unsigned char *)p1;
		size_t total = (size_t)NR_PAGES * 4096;
		size_t i;

		for (i = 0; i < total; i++)
			c[i] = (unsigned char)(i * 31u + 7u);
		for (i = 0; i < total; i++)
			if (c[i] != (unsigned char)(i * 31u + 7u)) {
				fprintf(stderr,
					"FAIL: pattern mismatch at byte %zu\n",
					i);
				return 1;
			}
	}
	fprintf(stderr, "ok: pattern readback through mmap'd pointer\n");

	/* 4. second mmap: already mapped, same pointer, no double-map */
	rc = linuxu_ttm_bo_shim_mmap(bo, &p2);
	EXPECT(rc == -17);
	EXPECT(p2 == p1);

	/* the pattern written through p1 is still intact (single backing) */
	{
		volatile unsigned char *c = (volatile unsigned char *)p1;
		size_t i;

		for (i = 0; i < (size_t)NR_PAGES * 4096; i++)
			if (c[i] != (unsigned char)(i * 31u + 7u)) {
				fprintf(stderr,
					"FAIL: pattern lost after 2nd mmap\n");
				return 1;
			}
	}

	/* 5. a later fault is the no-op "already mapped" */
	EXPECT(linuxu_ttm_bo_shim_fault(bo) == 0);
	EXPECT(linuxu_ttm_bo_shim_fault(bo) == 0);

	/* teardown */
	linuxu_ttm_bo_shim_unmap(bo);
	EXPECT(linuxu_ttm_bo_shim_fault(bo) == -2); /* unmapped again */
	linuxu_ttm_bo_shim_destroy(bo);

	/* kmemcheck: backing pages were kmemalloc'd and must be clean */
	EXPECT(kmemcheck_verify_all() == 0);
	fprintf(stderr, "PASS test_bo_mmap\n");
	return 0;
}
