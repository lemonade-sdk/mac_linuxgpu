/* Per-thread errno storage for DriverKit C compatibility. DriverKit.framework
 * provides thread-local storage but does not export libSystem's __error(). */
#ifdef LINUXU_DEXT_DK

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <rt/fatal.h>

extern int IOThreadLocalStorageKeyCreate(uint64_t *key);
extern int IOThreadLocalStorageSet(uint64_t key, const void *value);
extern void *IOThreadLocalStorageGet(uint64_t key);
extern void IODelay(uint64_t microseconds);
extern void *IOMalloc(size_t length);
extern void IOFree(void *address, size_t length);

static atomic_int errno_key_state;
static uint64_t errno_key;

int *__error(void)
{
	int state = atomic_load_explicit(&errno_key_state, memory_order_acquire);
	if (state == 0) {
		int expected = 0;
		if (atomic_compare_exchange_strong_explicit(&errno_key_state,
				&expected, 1, memory_order_acq_rel,
				memory_order_acquire)) {
			if (IOThreadLocalStorageKeyCreate(&errno_key) != 0) {
				/* Publish failure so waiters stop polling and
				 * reach the fatal path instead of IODelay spinning. */
				atomic_store_explicit(&errno_key_state, 3,
							memory_order_release);
				LINUXU_FATAL("errno TLS key unavailable");
			}
			atomic_store_explicit(&errno_key_state, 2,
						memory_order_release);
			state = 2;
		} else {
			state = expected;
		}
	}
	while (state == 1) {
		IODelay(1);
		state = atomic_load_explicit(&errno_key_state, memory_order_acquire);
	}
	if (state != 2)
		LINUXU_FATAL("errno TLS key unavailable");

	int *value = IOThreadLocalStorageGet(errno_key);
	if (!value) {
		value = IOMalloc(sizeof(*value));
		if (!value)
			LINUXU_FATAL("errno TLS allocation failed");
		*value = 0;
		if (IOThreadLocalStorageSet(errno_key, value) != 0) {
			IOFree(value, sizeof(*value));
			LINUXU_FATAL("errno TLS store failed");
		}
	}
	return value;
}

#endif
