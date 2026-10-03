/* Xarray ownership/bounds checks against the production heap adapter. */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <linux/xarray.h>
#include <linux/rcupdate.h>
#include "dext_heap_backend.h"

static void check_indices(void)
{
	struct xarray xa;
	xa_init(&xa);
	unsigned long indices[] = {0, 63, 64, 4095, 4096, 262144, 1UL << 40, ULONG_MAX};
	for (unsigned int i = 0; i < sizeof(indices)/sizeof(indices[0]); i++) {
		assert(!xa_err(xa_store(&xa, indices[i], xa_mk_value(i + 1), 0)));
		for (unsigned int j = 0; j <= i; j++)
			assert(xa_load(&xa, indices[j]) == xa_mk_value(j + 1));
	}
	assert(!xa_load(&xa, 1UL << 50));
	unsigned long index;
	void *entry;
	unsigned int seen = 0;
	xa_for_each(&xa, index, entry) {
		assert(seen < sizeof(indices)/sizeof(indices[0]));
		assert(index == indices[seen] && entry == xa_mk_value(seen + 1));
		seen++;
	}
	assert(seen == sizeof(indices)/sizeof(indices[0]));
	assert(xa_count(&xa, 0, ULONG_MAX) == seen);
	assert(xa_store(&xa, 4096, xa_mk_value(99), 0) == xa_mk_value(5));
	for (unsigned int i = 0; i < seen; i++) assert(xa_erase(&xa, indices[i]));
	assert(xa_empty(&xa));
	xa_destroy(&xa);
}

static void check_cyclic_guards(void)
{
	struct xarray xa;
	xa_init(&xa);
	struct { u32 before, id, after; } id = {0xdeadbeef, 0, 0xcafebabe};
	struct { u32 before, next, after; } next = {0xbaddecaf, 0, 0x13572468};
	for (u32 expected = 128; expected <= 130; expected++) {
		assert(xa_alloc_cyclic_irq(&xa, &id.id, NULL, XA_LIMIT(128, 130), &next.next, 0) == 0);
		assert(id.id == expected);
		assert(id.before == 0xdeadbeef && id.after == 0xcafebabe);
		assert(next.before == 0xbaddecaf && next.after == 0x13572468);
	}
	assert(xa_alloc_cyclic_irq(&xa, &id.id, NULL, XA_LIMIT(128, 130), &next.next, 0) == -EBUSY);
	xa_erase(&xa, 129);
	assert(xa_alloc_cyclic_irq(&xa, &id.id, xa_mk_value(128), XA_LIMIT(128, 130), &next.next, 0) == 0);
	assert(id.id == 129 && xa_load(&xa, 129) == xa_mk_value(128));
	xa_destroy(&xa);
	unsigned long maximum = ULONG_MAX;
	assert(xa_alloc_max(&xa, &maximum, NULL, ULONG_MAX, 0) == 0);
	maximum = ULONG_MAX;
	assert(xa_alloc_max(&xa, &maximum, NULL, ULONG_MAX, 0) == -ENOSPC);
	xa_destroy(&xa);
}

static void check_failure_and_marks(void)
{
	for (long fail = 0; fail < 22; fail++) {
		struct xarray xa;
		xa_init(&xa);
		assert(!xa_store(&xa, 3, xa_mk_value(7), 0));
		size_t baseline = dext_heap_test_live_allocations();
		dext_heap_test_fail_after(fail);
		void *result = xa_store(&xa, ULONG_MAX, xa_mk_value(8), 0);
		dext_heap_test_fail_after(-1);
		assert(xa_load(&xa, 3) == xa_mk_value(7));
		if (xa_is_err(result)) {
			assert(xa_err(result) == -ENOMEM && !xa_load(&xa, ULONG_MAX));
			assert(dext_heap_test_live_allocations() == baseline);
		} else assert(xa_load(&xa, ULONG_MAX) == xa_mk_value(8));
		xa_destroy(&xa);
		rcu_barrier();
	}
	struct xarray first, second;
	xa_init(&first); xa_init(&second);
	assert(!xa_store(&first, 9, xa_mk_value(1), 0));
	assert(!xa_store(&second, 9, xa_mk_value(2), 0));
	assert(xa_set_mark(&first, 9, 0, 0) == 0);
	assert(xa_test_mark(&first, 9, 0) && !xa_test_mark(&second, 9, 0));
	xa_clear_mark(&first, 9, 0);
	assert(!xa_test_mark(&first, 9, 0));
	xa_set_mark(&first, 9, 2, 0); xa_erase(&first, 9);
	assert(!xa_test_mark(&first, 9, 2));
	xa_destroy(&first); xa_destroy(&second);
	/* Empty reads/destruction must not consume a finite global side table. */
	for (unsigned int i = 0; i < 600; i++) {
		struct xarray array;
		xa_init(&array); assert(!xa_load(&array, 3));
		assert(!xa_store(&array, 3, xa_mk_value(3), 0));
		xa_destroy(&array);
	}
}

static void check_release_iteration(void)
{
	struct xarray xa;
	xa_init(&xa);
	for (unsigned int i = 0; i < 4; i++) {
		unsigned int *item = malloc(sizeof(*item));
		assert(item); *item = i;
		assert(!xa_store(&xa, i * 5000, item, 0));
	}
	unsigned long index;
	unsigned int *item, seen = 0;
	/* Scheduler cleanup releases entries before destroying the xarray. */
	xa_for_each(&xa, index, item) {
		assert(*item == seen++);
		free(item);
	}
	assert(seen == 4);
	xa_destroy(&xa);
}

static void check_locked_iteration(void)
{
	struct { struct xarray entries; } embedded = { XA_INIT(entries) };
	assert(!xa_store(&embedded.entries, 64, xa_mk_value(9), 0));
	XA_STATE(state, &embedded.entries, 64);
	xas_lock(&state);
	assert(xas_load(&state) == xa_mk_value(9));
	assert(xas_erase(&state) == xa_mk_value(9));
	xas_unlock(&state);
	assert(xa_empty(&embedded.entries));
	xa_destroy(&embedded.entries);
}

int main(void)
{
	size_t baseline = dext_heap_test_live_allocations();
	check_indices(); check_cyclic_guards(); rcu_barrier(); check_failure_and_marks(); check_release_iteration();
	check_locked_iteration();
	rcu_barrier();
	assert(dext_heap_test_live_allocations() == baseline);
	puts("xarray: cyclic u32 guards, sparse bounds, cleanup iteration, marks, 22 OOM budgets passed");
}
