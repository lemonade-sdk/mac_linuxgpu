/* test_kmemcheck.c — kmemcheck canary detection (REAL): corrupt a
 * byte past the payload → kmemcheck_verify_all() must flag it. */
#include <stdio.h>
#include <string.h>

#include <linux/slab.h>
#include <linux/gfp.h>

#define EXPECT(cond) do {						\
	if (!(cond)) {							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		return 1;						\
	} else {							\
		fprintf(stderr, "ok: %s\n", #cond);			\
	}								\
} while (0)

extern int kmemcheck_verify_all(void);
extern int kmemcheck_enabled(void);

int main(void)
{
	/* clean state: no corruption */
	EXPECT(kmemcheck_enabled());
	EXPECT(kmemcheck_verify_all() == 0);

	/* alloc + corrupt the trailing canary (1 byte past payload) */
	void *p = kmalloc(128, 0);
	EXPECT(p != NULL);
	memset(p, 0xab, 128);
	/* 1-byte OOB write past the payload hits canary_hi */
	((unsigned char *)p)[128] = 0x5a;
	EXPECT(kmemcheck_verify_all() == 1);

	/* corrupting inside the payload is not (yet) canary-covered */
	void *q = kmalloc(64, 0);
	memset(q, 1, 64);
	((unsigned char *)q)[60] = 0x99;
	/* still exactly one corruption (the p one) */
	EXPECT(kmemcheck_verify_all() == 1);

	/* freeing a corrupted object: kmemcheck_untrack reports 0 */
	/* (kfree does not crash either way) */
	kfree(p);
	kfree(q);
	EXPECT(kmemcheck_verify_all() == 0);
	fprintf(stderr, "PASS test_kmemcheck\n");
	return 0;
}
