#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include <linux/kref.h>

#ifdef LINUXU_DEXT_DK
static atomic64_t *const mmio_token = (atomic64_t *)(uintptr_t)0x40000000;
static u64 mmio_value;
static unsigned mmio_reads, mmio_writes;
int linuxu_atomic64_mmio_read(const volatile void *address, u64 *value)
{
	if (address != mmio_token) return 0;
	*value = mmio_value;
	mmio_reads++;
	return 1;
}
int linuxu_atomic64_mmio_write(volatile void *address, u64 value)
{
	if (address != mmio_token) return 0;
	mmio_value = value;
	mmio_writes++;
	return 1;
}
static void test_mmio_variants(void)
{
	/* A synthetic token is deliberately not mapped in the host process. */
	atomic64_set(mmio_token, 7);
	assert(atomic64_read(mmio_token) == 7);
	atomic64_set_release(mmio_token, 13);
	assert(atomic64_read_acquire(mmio_token) == 13);
	atomic64_set_relaxed(mmio_token, 19);
	assert(atomic64_read_relaxed(mmio_token) == 19);
	assert(mmio_reads == 3 && mmio_writes == 3);
}
#endif

static unsigned warnings;
void linuxu_warn(const char *file, int line, const char *fmt, ...)
{
	(void)file; (void)line; (void)fmt;
	warnings++;
}
void linuxu_bug(const char *file, int line)
{
	(void)file; (void)line;
	assert(!"unexpected fatal refcount path");
}

#define CHECK_CMP(operation, type, read, set) do { \
	type value; set(&value, 37); \
	assert(operation(&value, 37, 91) == 37); \
	assert(read(&value) == 91); \
	int expected = 37; \
	assert(operation(&value, expected, 113) == 91); \
	assert(expected == 37 && read(&value) == 91); \
} while (0)

#define CHECK_TRY(operation, type, scalar_type, read, set) do { \
	type value; set(&value, 37); scalar_type expected = 37; \
	assert(operation(&value, &expected, 91)); \
	assert(expected == 37 && read(&value) == 91); \
	assert(!operation(&value, &expected, 113)); \
	assert(expected == 91 && read(&value) == 91); \
} while (0)

static void test_compare_exchange(void)
{
	CHECK_CMP(atomic_cmpxchg, atomic_t, atomic_read, atomic_set);
	CHECK_CMP(atomic_cmpxchg_relaxed, atomic_t, atomic_read, atomic_set);
	CHECK_CMP(atomic_cmpxchg_acquire, atomic_t, atomic_read, atomic_set);
	CHECK_CMP(atomic_cmpxchg_release, atomic_t, atomic_read, atomic_set);
	CHECK_CMP(atomic64_cmpxchg, atomic64_t, atomic64_read, atomic64_set);
	CHECK_CMP(atomic64_cmpxchg_relaxed, atomic64_t, atomic64_read, atomic64_set);
	CHECK_CMP(atomic64_cmpxchg_acquire, atomic64_t, atomic64_read, atomic64_set);
	CHECK_CMP(atomic64_cmpxchg_release, atomic64_t, atomic64_read, atomic64_set);
	CHECK_CMP(atomic_long_cmpxchg, atomic_long_t, atomic_long_read, atomic_long_set);
	CHECK_TRY(atomic_try_cmpxchg, atomic_t, int, atomic_read, atomic_set);
	CHECK_TRY(atomic_try_cmpxchg_relaxed, atomic_t, int, atomic_read, atomic_set);
	CHECK_TRY(atomic_try_cmpxchg_acquire, atomic_t, int, atomic_read, atomic_set);
	CHECK_TRY(atomic_try_cmpxchg_release, atomic_t, int, atomic_read, atomic_set);
	CHECK_TRY(atomic64_try_cmpxchg, atomic64_t, long long, atomic64_read, atomic64_set);
	CHECK_TRY(atomic64_try_cmpxchg_relaxed, atomic64_t, long long, atomic64_read, atomic64_set);
	CHECK_TRY(atomic64_try_cmpxchg_acquire, atomic64_t, long long, atomic64_read, atomic64_set);
	CHECK_TRY(atomic64_try_cmpxchg_release, atomic64_t, long long, atomic64_read, atomic64_set);
	int raw[2] = {37, 0}, *pointer = raw;
	assert(cmpxchg(pointer++, 37, 91) == 37 && pointer == raw + 1);
	assert(cmpxchg_relaxed(raw, 37, 91) == 91);
	assert(cmpxchg_acquire(raw, 91, 113) == 91);
	assert(cmpxchg_release(raw, 113, 37) == 113);
	assert(xchg(raw, 91) == 37 && xchg_relaxed(raw, 113) == 91);
	int *old_pointer = raw;
	assert(cmpxchg(&old_pointer, raw, raw + 1) == raw);
	assert(old_pointer == raw + 1);
}

static void test_conditional_changes(void)
{
	atomic_t value = ATOMIC_INIT(0);
	assert(!atomic_inc_not_zero(&value) && atomic_read(&value) == 0);
	assert(!atomic_inc_not_zero_acquire(&value) && atomic_read(&value) == 0);
	assert(atomic_dec_if_positive(&value) == -1 && atomic_read(&value) == 0);
	atomic_set(&value, 1);
	assert(atomic_dec_if_positive(&value) == 0 && atomic_read(&value) == 0);
	atomic_set(&value, -5);
	assert(atomic_dec_if_positive(&value) == -6 && atomic_read(&value) == -5);
	atomic_set(&value, 7);
	assert(!atomic_add_unless(&value, 2, 7) && atomic_read(&value) == 7);
	assert(atomic_add_unless(&value, 2, 2) && atomic_read(&value) == 9);
	assert(!atomic_add_unless_acquire(&value, 2, 9));
	assert(atomic_add_unless_release(&value, -9, 2) && atomic_read(&value) == 0);
	atomic64_t wide = ATOMIC64_INIT(0);
	assert(!atomic64_inc_not_zero(&wide) && atomic64_read(&wide) == 0);
	atomic64_set(&wide, 99);
	assert(atomic64_inc_not_zero(&wide) && atomic64_read(&wide) == 100);
}

static unsigned releases;
static void release_kref(struct kref *ref) { (void)ref; releases++; }

static void test_lifetimes(void)
{
	refcount_t refs = REFCOUNT_INIT(0);
	assert(!refcount_inc_not_zero(&refs) && refcount_read(&refs) == 0);
	assert(!refcount_inc_not_zero_many(&refs, 17) && refcount_read(&refs) == 0);
	assert(!refcount_dec_if_not_zero(&refs) && refcount_read(&refs) == 0);
	refcount_set(&refs, 1);
	assert(!refcount_dec_not_one(&refs) && refcount_read(&refs) == 1);
	assert(refcount_inc_not_zero_many(&refs, 17) && refcount_read(&refs) == 18);
	assert(refcount_dec_not_one(&refs) && refcount_read(&refs) == 17);
	assert(refcount_dec_and_test_many(&refs, 17) && refcount_read(&refs) == 0);
	refcount_set(&refs, REFCOUNT_MAX);
	assert(refcount_inc_not_zero_many(&refs, 2));
	assert(refcount_read(&refs) == REFCOUNT_SATURATE);
	assert(!refcount_dec_and_test(&refs) && refcount_read(&refs) == REFCOUNT_SATURATE);
	refcount_set(&refs, 0);
	assert(!refcount_dec_and_test(&refs) && refcount_read(&refs) == REFCOUNT_SATURATE);
	assert(warnings == 2);
	struct kref k;
	kref_init(&k);
	kref_get(&k);
	kref_put(&k, release_kref);
	assert(releases == 0 && kref_read(&k) == 1);
	kref_put(&k, release_kref);
	assert(releases == 1 && !kref_get_unless_zero(&k) && kref_read(&k) == 0);
	kref_init(&k);
	refcount_inc_not_zero_many(&k.refcount, 3);
	__kref_put(&k, 4, release_kref);
	assert(releases == 2 && kref_read(&k) == 0);
}

static atomic_t sequences = ATOMIC_INIT(37);
static refcount_t shared_refs = REFCOUNT_INIT(1);
static void *worker(void *context)
{
	(void)context;
	for (int i = 0; i < 4000; i++) {
		int old, attempts = 0;
		do {
			assert(++attempts < 1000000);
			old = atomic_read(&sequences);
		} while (atomic_cmpxchg(&sequences, old, old + 1) != old);
		assert(refcount_inc_not_zero_many(&shared_refs, 7));
		assert(!refcount_dec_and_test_many(&shared_refs, 7));
	}
	return NULL;
}

int main(void)
{
#ifdef LINUXU_DEXT_DK
	test_mmio_variants();
#endif
	test_compare_exchange();
	test_conditional_changes();
	test_lifetimes();
	pthread_t threads[4];
	for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
	for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
	assert(atomic_read(&sequences) == 16037 && refcount_read(&shared_refs) == 1);
	assert(refcount_dec_and_test(&shared_refs));
	puts("atomic/refcount semantics: compare-exchange, zero retention, saturation and concurrent fence/ref retries passed");
	return 0;
}
