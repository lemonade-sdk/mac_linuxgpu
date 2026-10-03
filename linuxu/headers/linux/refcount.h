/* linuxu: SHIM (third_party/linux/include/linux/refcount.h)
 * refcount_t built on atomic_t. Runtime: linuxu/src/kmem/refcount.c
 */
#ifndef _LINUX_REFCOUNT_H
#define _LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/limits.h>

typedef struct {
	atomic_t count;
} refcount_t;

#define REFCOUNT_INIT(i)	{ .count = ATOMIC_INIT(i) }
#define REFCOUNT_SATURATE	INT_MAX
#define REFCOUNT_MAX		(INT_MAX - 1)

static inline void refcount_set(refcount_t *r, unsigned int n)
{
	atomic_set(&r->count, (int)n);
}

static inline bool refcount_inc_not_zero_many(refcount_t *r, unsigned int n)
{
	int current = atomic_read(&r->count);
	for (;;) {
		if (current == 0)
			return false;
		if (current == REFCOUNT_SATURATE)
			return true;
		int next;
		if (current < 0 || n > (unsigned int)(REFCOUNT_MAX - current)) {
			WARN(1, "refcount_t saturation\n");
			next = REFCOUNT_SATURATE;
		} else {
			next = current + (int)n;
		}
		if (atomic_try_cmpxchg(&r->count, &current, next))
			return true;
	}
}

static inline int refcount_inc_not_zero(refcount_t *r)
{
	return refcount_inc_not_zero_many(r, 1);
}

static inline void refcount_inc(refcount_t *r)
{
	if (unlikely(!refcount_inc_not_zero(r))) {
		WARN(1, "refcount_t increment from zero\n");
		atomic_set(&r->count, REFCOUNT_SATURATE);
	}
}

static inline bool refcount_dec_and_test_many(refcount_t *r, unsigned int n)
{
	int current = atomic_read(&r->count);
	for (;;) {
		if (current == REFCOUNT_SATURATE)
			return false;
		if (current < 0 || n > (unsigned int)current) {
			WARN(1, "refcount_t underflow\n");
			if (atomic_try_cmpxchg(&r->count, &current, REFCOUNT_SATURATE))
				return false;
		} else {
			int next = current - (int)n;
			if (atomic_try_cmpxchg_release(&r->count, &current, next)) {
				if (!next)
					__atomic_thread_fence(__ATOMIC_ACQUIRE);
				return next == 0;
			}
		}
	}
}

static inline bool refcount_dec_and_test(refcount_t *r)
{
	return refcount_dec_and_test_many(r, 1);
}

static inline bool refcount_dec_not_one(refcount_t *r)
{
	int current = atomic_read(&r->count);
	for (;;) {
		if (current == 1)
			return false;
		if (current == REFCOUNT_SATURATE)
			return true;
		if (current <= 0) {
			WARN(1, "refcount_t underflow\n");
			if (atomic_try_cmpxchg(&r->count, &current, REFCOUNT_SATURATE))
				return true;
		} else if (atomic_try_cmpxchg_release(&r->count, &current, current - 1)) {
			return true;
		}
	}
}

static inline bool refcount_dec_if_not_zero(refcount_t *r)
{
	int current = atomic_read(&r->count);
	for (;;) {
		if (current == 0)
			return false;
		if (current == REFCOUNT_SATURATE)
			return true;
		if (current < 0) {
			WARN(1, "refcount_t underflow\n");
			return false;
		}
		if (atomic_try_cmpxchg_release(&r->count, &current, current - 1))
			return true;
	}
}

static inline int refcount_read(const refcount_t *r)
{
	return atomic_read(&r->count);
}

static inline bool refcount_zero(const refcount_t *r)
{
	return atomic_read(&r->count) == 0;
}

#endif /* _LINUX_REFCOUNT_H */
