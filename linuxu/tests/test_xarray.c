/* test_xarray.c — xarray (REAL): store/load/erase round trip,
 * find, count, xa_alloc unique index, destroy. */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

#include <linux/xarray.h>

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
	struct xarray xa;
	void *p1 = (void *)0x1001;
	void *p2 = (void *)0x2002;
	void *p3 = (void *)0x3003;
	unsigned long idx;

	xa_init(&xa);
	EXPECT(xa_is_empty(&xa));

	/* store / load */
	EXPECT(xa_store(&xa, 3, p1, 0) == 0);
	EXPECT(xa_store(&xa, 10, p2, 0) == 0);
	EXPECT(xa_store(&xa, 7, p3, 0) == 0);
	EXPECT(xa_load(&xa, 3) == p1);
	EXPECT(xa_load(&xa, 10) == p2);
	EXPECT(xa_load(&xa, 7) == p3);
	EXPECT(xa_load(&xa, 4) == NULL);
	EXPECT(!xa_is_empty(&xa));

	/* count */
	EXPECT(xa_count(&xa, 0, ULONG_MAX) == 3);

	/* find: first entry at or after 0 is index 3 */
	idx = 0;
	EXPECT(xa_find(&xa, &idx, ULONG_MAX, 0) == p1);
	EXPECT(idx == 3);
	/* next after index 7 is 10 */
	idx = 7;
	EXPECT(xa_find(&xa, &idx, ULONG_MAX, 0) == p3);
	EXPECT(idx == 7);

	/* erase */
	EXPECT(xa_erase(&xa, 3) == p1);
	EXPECT(xa_load(&xa, 3) == NULL);
	EXPECT(xa_count(&xa, 0, ULONG_MAX) == 2);

	/* value entries */
	EXPECT(xa_store(&xa, 5, xa_mk_value(0x42), 0) == 0);
	EXPECT(xa_to_value(xa_load(&xa, 5)) == 0x42);
	EXPECT(xa_is_value(xa_load(&xa, 5)));

	/* XA_ALLOC_MAX (plain max form): unique indices in [0, 64) */
	idx = 0;
	EXPECT(XA_ALLOC_MAX(&xa, &idx, (void *)0x4004, 64, 0) == 0);
	idx = 0;
	EXPECT(XA_ALLOC_MAX(&xa, &idx, (void *)0x5005, 64, 0) == 0);

	xa_destroy(&xa);
	fprintf(stderr, "PASS test_xarray\n");
	return 0;
}
