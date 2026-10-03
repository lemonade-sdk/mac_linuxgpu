/* linuxu: EDITED (third_party/linux/include/linux/atomic.h) - upstream
 * linux/atomic.h is a thin wrapper over <asm/atomic.h>; since the
 * asm tree is shim-owned, we implement the whole atomic family here
 * with C11 atomics (GCC/Clang builtins). The machine-independent
 * acquire/release/fence macro structure is kept from upstream. */
/* SPDX-License-Identifier: GPL-2.0 */
/* Atomic operations usable in machine independent code */
#ifndef _LINUX_ATOMIC_H
#define _LINUX_ATOMIC_H

#include <linux/types.h>
#include <stddef.h>

/*
 * Relaxed variants of xchg, cmpxchg and some atomic operations.
 * Upstream maps these onto __atomic_*_relaxed; here the plain
 * builtins are the fully-ordered variants and the _relaxed suffix
 * selects the C11 relaxed memory order.
 */

#define __atomic_acquire_fence	smp_mb__after_atomic
#define __atomic_release_fence	smp_mb__before_atomic
#define __atomic_pre_full_fence	smp_mb__before_atomic
#define __atomic_post_full_fence	smp_mb__after_atomic

/*
 * cmpxchg() (upstream <asm-generic/cmpxchg.h>): generic compare-and-swap on
 * an arbitrary integer pointer.  Used by the driver as a plain pointer CAS
 * (e.g. drm_sched_entity_flush).  Returns the previous value of *ptr.
 */
#define __linuxu_cmpxchg(ptr, o, n, success, failure) ({ \
	typeof(*(ptr)) __linuxu_old = (o); \
	(void)__atomic_compare_exchange_n((ptr), &__linuxu_old, (n), \
					  false, (success), (failure)); \
	__linuxu_old; })
#define cmpxchg(ptr, o, n) __linuxu_cmpxchg(ptr, o, n, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define cmpxchg_relaxed(ptr, o, n) __linuxu_cmpxchg(ptr, o, n, __ATOMIC_RELAXED, __ATOMIC_RELAXED)
#define cmpxchg_acquire(ptr, o, n) __linuxu_cmpxchg(ptr, o, n, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
#define cmpxchg_release(ptr, o, n) __linuxu_cmpxchg(ptr, o, n, __ATOMIC_RELEASE, __ATOMIC_RELAXED)
#define xchg(ptr, n) __atomic_exchange_n((ptr), (n), __ATOMIC_SEQ_CST)
#define xchg_relaxed(ptr, n) __atomic_exchange_n((ptr), (n), __ATOMIC_RELAXED)
#define try_cmpxchg(ptr, oldp, n) \
	__atomic_compare_exchange_n((ptr), (oldp), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define try_cmpxchg_relaxed(ptr, oldp, n) \
	__atomic_compare_exchange_n((ptr), (oldp), (n), false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)
#define try_cmpxchg_acquire(ptr, oldp, n) \
	__atomic_compare_exchange_n((ptr), (oldp), (n), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
#define try_cmpxchg_release(ptr, oldp, n) \
	__atomic_compare_exchange_n((ptr), (oldp), (n), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)

/*
 * Basic types: a C11 atomic of the right width.
 */
#ifndef _LINUX_TYPES_ATOMIC_T_DEFINED
typedef struct {
	int counter;
} atomic_t;
#define ATOMIC_INIT(i) { (i) }
#endif

#ifndef __LINUXU_ATOMIC64_T_DEFINED
typedef struct {
	long long counter;
} atomic64_t;
#define ATOMIC64_INIT(i) { (i) }
#define __LINUXU_ATOMIC64_T_DEFINED
#endif


#define atomic_read(v)		(__atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST))
#define atomic_set(v, i)	(__atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_set_release(v, i)	(__atomic_store_n(&(v)->counter, (i), __ATOMIC_RELEASE))
#define atomic_read_acquire(v)	(smp_load_acquire(&(v)->counter))

#define atomic_inc(v)		((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec(v)		((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_add(i, v)	(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_sub(i, v)	(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_inc_return(v)	(__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec_return(v)	(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
static inline int atomic_dec_if_positive(atomic_t *v)
{
	int current = atomic_read(v), next;
	do {
		next = (int)((unsigned int)current - 1u);
		if (next < 0)
			return next;
	} while (!__atomic_compare_exchange_n(&v->counter, &current, next, false,
					     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	return next;
}
static inline bool __linuxu_atomic_add_unless(atomic_t *v, int add, int unless,
					     int order)
{
	int current = __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
	while (current != unless) {
		int next = (int)((unsigned int)current + (unsigned int)add);
		if (__atomic_compare_exchange_n(&v->counter, &current, next, false,
						order, __ATOMIC_RELAXED))
			return true;
	}
	return false;
}
#define atomic_inc_not_zero(v) __linuxu_atomic_add_unless((v), 1, 0, __ATOMIC_SEQ_CST)
#define atomic_dec_and_test(v)	(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_SEQ_CST) == 1)
#define atomic_add_return(i, v)	(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_sub_return(i, v)	(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_add_unless(v, a, u) __linuxu_atomic_add_unless((v), (a), (u), __ATOMIC_SEQ_CST)
#define atomic_fetch_add(i, v)	(__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_fetch_sub(i, v)	(__atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_fetch_inc(v)	(__atomic_fetch_add(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_fetch_dec(v)	(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_SEQ_CST))

#define atomic_xchg(v, i)	(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_SEQ_CST))
static inline int __linuxu_atomic_cmpxchg(atomic_t *v, int old, int new)
{
	__atomic_compare_exchange_n(&v->counter, &old, new,
					   false, __ATOMIC_SEQ_CST,
					   __ATOMIC_SEQ_CST);
	return old;
}
#define atomic_cmpxchg(v, o, n) __linuxu_atomic_cmpxchg((v), (o), (n))
#define atomic_try_cmpxchg(v, oldp, n) try_cmpxchg(&(v)->counter, oldp, n)

/* Relaxed variants */
#define atomic_read_relaxed(v)	(__atomic_load_n(&(v)->counter, __ATOMIC_RELAXED))
#define atomic_set_relaxed(v, i)	(__atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_add_relaxed(i, v)	(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_sub_relaxed(i, v)	(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_inc_relaxed(v)		((void)atomic_add_relaxed(1, v))
#define atomic_dec_relaxed(v)		((void)atomic_sub_relaxed(1, v))
#define atomic_inc_return_relaxed(v)	(__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic_dec_return_relaxed(v)	(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic_dec_and_test_relaxed(v)	(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_RELAXED) == 1)
#define atomic_fetch_add_relaxed(i, v)	(__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_fetch_sub_relaxed(i, v)	(__atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_fetch_inc_relaxed(v)	(__atomic_fetch_add(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic_fetch_dec_relaxed(v)	(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic_xchg_relaxed(v, i)	(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_cmpxchg_relaxed(v, o, n) \
	cmpxchg_relaxed(&(v)->counter, (o), (n))
#define atomic_try_cmpxchg_relaxed(v, oldp, n) try_cmpxchg_relaxed(&(v)->counter, oldp, n)
#define atomic_add_return_relaxed(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_sub_return_relaxed(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_RELAXED)

/* Acquire/release variants built on the relaxed ones, as upstream does */
#define __atomic_op_acquire(op, args...)				\
({									\
	typeof(op##_relaxed(args)) __ret  = op##_relaxed(args);	\
	__atomic_acquire_fence();					\
	__ret;								\
})

#define __atomic_op_release(op, args...)				\
({									\
	__atomic_release_fence();					\
	op##_relaxed(args);						\
})

#define __atomic_op_fence(op, args...)				\
({									\
	typeof(op##_relaxed(args)) __ret;				\
	__atomic_pre_full_fence();					\
	__ret = op##_relaxed(args);					\
	__atomic_post_full_fence();					\
	__ret;								\
})

#define atomic_xchg_acquire(v, i)	(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_ACQUIRE))
#define atomic_xchg_release(v, i)	(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_RELEASE))
#define atomic_cmpxchg_acquire(v, o, n) \
	cmpxchg_acquire(&(v)->counter, (o), (n))
#define atomic_cmpxchg_release(v, o, n) \
	cmpxchg_release(&(v)->counter, (o), (n))
#define atomic_try_cmpxchg_acquire(v, oldp, n) try_cmpxchg_acquire(&(v)->counter, oldp, n)
#define atomic_try_cmpxchg_release(v, oldp, n) try_cmpxchg_release(&(v)->counter, oldp, n)
#define atomic_add_return_acquire(i, v) __atomic_op_acquire(atomic_add_return, i, v)
#define atomic_add_return_release(i, v) __atomic_op_release(atomic_add_return, i, v)
#define atomic_add_unless_acquire(v, a, u) \
	__linuxu_atomic_add_unless((v), (a), (u), __ATOMIC_ACQUIRE)
#define atomic_add_unless_release(v, a, u) \
	__linuxu_atomic_add_unless((v), (a), (u), __ATOMIC_RELEASE)
#define atomic_inc_not_zero_acquire(v) \
	__linuxu_atomic_add_unless((v), 1, 0, __ATOMIC_ACQUIRE)

/* 64-bit variants */
#ifdef LINUXU_DEXT_DK
/* BAR mappings are synthetic addresses.  The upstream doorbell helpers use
 * atomic64_read/set on those addresses, so route just live MMIO tokens to
 * the PCI bridge.  Ordinary atomics remain CPU memory operations. */
extern int linuxu_atomic64_mmio_read(const volatile void *addr, u64 *value);
extern int linuxu_atomic64_mmio_write(volatile void *addr, u64 value);
static inline long long linuxu_atomic64_read_order(const atomic64_t *v, int order)
{
	u64 value;
	if (linuxu_atomic64_mmio_read(&v->counter, &value)) {
		__atomic_thread_fence(order);
		return (long long)value;
	}
	return __atomic_load_n(&v->counter, order);
}
static inline void linuxu_atomic64_set_order(atomic64_t *v, long long value, int order)
{
	__atomic_thread_fence(order);
	if (!linuxu_atomic64_mmio_write(&v->counter, (u64)value))
		__atomic_store_n(&v->counter, value, order);
}
#define atomic64_read(v) linuxu_atomic64_read_order((v), __ATOMIC_SEQ_CST)
#define atomic64_set(v, i) linuxu_atomic64_set_order((v), (i), __ATOMIC_SEQ_CST)
#define atomic64_set_release(v, i) linuxu_atomic64_set_order((v), (i), __ATOMIC_RELEASE)
#define atomic64_read_acquire(v) linuxu_atomic64_read_order((v), __ATOMIC_ACQUIRE)
#define atomic64_read_relaxed(v) linuxu_atomic64_read_order((v), __ATOMIC_RELAXED)
#define atomic64_set_relaxed(v, i) linuxu_atomic64_set_order((v), (i), __ATOMIC_RELAXED)
#else
#define atomic64_read(v)		(__atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST))
#define atomic64_set(v, i)		(__atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_set_release(v, i)	(smp_store_release(&(v)->counter, (i)))
#define atomic64_read_acquire(v)	(smp_load_acquire(&(v)->counter))
#define atomic64_read_relaxed(v)		(__atomic_load_n(&(v)->counter, __ATOMIC_RELAXED))
#define atomic64_set_relaxed(v, i)		(__atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED))
#endif
#define atomic64_inc(v)			((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_dec(v)			((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_add(i, v)		(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_sub(i, v)		(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_inc_return(v)		(__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_dec_return(v)		(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
static inline bool atomic64_inc_not_zero(atomic64_t *v)
{
	long long current = __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
	while (current != 0) {
		long long next = (long long)((unsigned long long)current + 1ull);
		if (__atomic_compare_exchange_n(&v->counter, &current, next, false,
						__ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
			return true;
	}
	return false;
}
#define atomic64_dec_and_test(v)	(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_SEQ_CST) == 1)
#define atomic64_add_return(i, v)	(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_sub_return(i, v)	(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_fetch_add(i, v)	(__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_fetch_sub(i, v)	(__atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_fetch_inc(v)		(__atomic_fetch_add(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_fetch_dec(v)		(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_xchg(v, i)		(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_cmpxchg(v, o, n) cmpxchg(&(v)->counter, (o), (n))
#define atomic64_try_cmpxchg(v, oldp, n) try_cmpxchg(&(v)->counter, oldp, n)
#define atomic64_add_relaxed(i, v)		(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic64_sub_relaxed(i, v)		(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic64_inc_relaxed(v)		((void)atomic64_add_relaxed(1, v))
#define atomic64_dec_relaxed(v)		((void)atomic64_sub_relaxed(1, v))
#define atomic64_inc_return_relaxed(v)		(__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic64_dec_return_relaxed(v)		(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_RELAXED))
#define atomic64_dec_and_test_relaxed(v)		(__atomic_fetch_sub(&(v)->counter, 1, __ATOMIC_RELAXED) == 1)
#define atomic64_fetch_add_relaxed(i, v)		(__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic64_fetch_sub_relaxed(i, v)		(__atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic64_xchg_relaxed(v, i)		(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic64_cmpxchg_relaxed(v, o, n) \
	cmpxchg_relaxed(&(v)->counter, (o), (n))
#define atomic64_try_cmpxchg_relaxed(v, oldp, n) try_cmpxchg_relaxed(&(v)->counter, oldp, n)
#define atomic64_xchg_acquire(v, i)		(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_ACQUIRE))
#define atomic64_xchg_release(v, i)		(__atomic_exchange_n(&(v)->counter, (i), __ATOMIC_RELEASE))
#define atomic64_cmpxchg_acquire(v, o, n) \
	cmpxchg_acquire(&(v)->counter, (o), (n))
#define atomic64_cmpxchg_release(v, o, n) \
	cmpxchg_release(&(v)->counter, (o), (n))
#define atomic64_try_cmpxchg_acquire(v, oldp, n) try_cmpxchg_acquire(&(v)->counter, oldp, n)
#define atomic64_try_cmpxchg_release(v, oldp, n) try_cmpxchg_release(&(v)->counter, oldp, n)
#define atomic64_add_return_relaxed(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic64_sub_return_relaxed(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic64_add_return_acquire(i, v) __atomic_op_acquire(atomic64_add_return, i, v)
#define atomic64_add_return_release(i, v) __atomic_op_release(atomic64_add_return, i, v)

/* cond_read: smp_cond_load is not needed by the driver set; keep upstream
 * names mapping onto simple loops. */
#define atomic_cond_read_acquire(v, c)	smp_cond_load_acquire(&(v)->counter, (c))
#define atomic_cond_read_relaxed(v, c)	smp_cond_load_relaxed(&(v)->counter, (c))
#define atomic64_cond_read_acquire(v, c)	smp_cond_load_acquire(&(v)->counter, (c))
#define atomic64_cond_read_relaxed(v, c)	smp_cond_load_relaxed(&(v)->counter, (c))

/* long atomics used by the driver (ttm/ttm_pool.c etc.) */
/* Upstream: typedef struct { long counter; } atomic_long_t; */
typedef struct {
	long counter;
} atomic_long_t;
#define LONGATOMIC_INIT(i) { (i) }
#define atomic_long_read(v)		(__atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST))
#define atomic_long_set(v, i)		(__atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_long_add(i, v)		(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_long_sub(i, v)		(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_long_inc(v)		((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_long_dec(v)		((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_long_inc_return(v)		(__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_long_dec_return(v)		(__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_long_add_return(i, v)	(__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_long_sub_return(i, v)	(__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))

static inline long atomic_long_xchg(atomic_long_t *v, long new_val)
{
	return __atomic_exchange_n(&v->counter, new_val, __ATOMIC_SEQ_CST);
}
static inline long atomic_long_cmpxchg(atomic_long_t *v, long old, long new)
{
	__atomic_compare_exchange_n(&v->counter, &old, new, false,
				   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return old;
}

#endif /* _LINUX_ATOMIC_H */
