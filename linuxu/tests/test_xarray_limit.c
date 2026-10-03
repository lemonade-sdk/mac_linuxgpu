#include <assert.h>
#include <linux/xarray.h>
#include <linux/errno.h>

/* Single-threaded test; xarray's lock behavior is covered separately. */
void spin_lock(spinlock_t *lock) { (void)lock; }
void spin_unlock(spinlock_t *lock) { (void)lock; }

int main(void)
{
	struct xarray xa = { 0 };
	struct {
		u32 id;
		u32 guard;
	} result = { .id = 0, .guard = 0xa5a5a5a5 };

	assert(xa_alloc_limit(&xa, &result.id, NULL,
			      XA_LIMIT(128, 129), 0) == 0);
	assert(result.id == 128 && result.guard == 0xa5a5a5a5);
	assert(xa_load(&xa, 128) == NULL); /* reserved NULL stays hidden */
	assert(xa_alloc_limit(&xa, &result.id, NULL,
			      XA_LIMIT(128, 129), 0) == 0);
	assert(result.id == 129);
	assert(xa_alloc_limit(&xa, &result.id, NULL,
			      XA_LIMIT(128, 129), 0) == -EBUSY);
	assert(xa_erase(&xa, 128) == NULL);
	assert(xa_alloc_limit(&xa, &result.id, (void *)0x1000,
			      XA_LIMIT(128, 129), 0) == 0);
	assert(result.id == 128);
	assert(xa_load(&xa, 128) == (void *)0x1000);
	assert(xa_alloc_limit(&xa, &result.id, NULL,
			      XA_LIMIT(0, 1), 0) == 0);
	assert(result.id == 0);
	assert(xa_load(&xa, 128) == (void *)0x1000);
	assert(!xa_is_empty(&xa));
	assert(xa_erase(&xa, 128) == (void *)0x1000);
	assert(xa_erase(&xa, 129) == NULL);
	assert(xa_erase(&xa, 0) == NULL);
	assert(xa_is_empty(&xa));
	assert(xa_alloc_limit(&xa, &result.id, NULL,
			      XA_LIMIT(3, 2), 0) == -EINVAL);
	assert(xa_destroy(&xa) == 0);
	return 0;
}
