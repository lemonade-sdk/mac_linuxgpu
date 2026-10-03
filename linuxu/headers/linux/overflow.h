/* linuxu: SHIM (third_party/linux/include/linux/overflow.h)
 * Overflow-checking arithmetic via Clang __builtin_..._overflow.
 */
#ifndef __LINUX_OVERFLOW_H
#define __LINUX_OVERFLOW_H

#include <linux/types.h>
#include <linux/build_bug.h>
#include <math.h>

#define CHECK_ADD_OVF(x, y, res)	__builtin_add_overflow(x, y, (res))
#define CHECK_SUB_OVF(x, y, res)	__builtin_sub_overflow(x, y, (res))
#define CHECK_MUL_OVF(x, y, res)	__builtin_mul_overflow(x, y, (res))
#define CHECK_NEG(x, res)		__builtin_neg_overflow(x, (res))

#define check_add_overflow(x, y, res)	CHECK_ADD_OVF(x, y, res)
#define check_sub_overflow(x, y, res)	CHECK_SUB_OVF(x, y, res)
#define check_mul_overflow(x, y, res)	CHECK_MUL_OVF(x, y, res)
#define check_neg_overflow(x, res)	CHECK_NEG(x, res)

#define check_add_overflow_kernel(x, y, res)	check_add_overflow(x, y, res)
#define check_sub_overflow_kernel(x, y, res)	check_sub_overflow(x, y, res)
#define check_mul_overflow_kernel(x, y, res)	check_mul_overflow(x, y, res)

#define add_overflow(x, y, res)	CHECK_ADD_OVF(x, y, res)
#define sub_overflow(x, y, res)	CHECK_SUB_OVF(x, y, res)
#define mul_overflow(x, y, res)	CHECK_MUL_OVF(x, y, res)

#define add_not_overflow(x, y, res)	(!CHECK_ADD_OVF(x, y, (res)))
#define sub_not_overflow(x, y, res)	(!CHECK_SUB_OVF(x, y, (res)))
#define mul_not_overflow(x, y, res)	(!CHECK_MUL_OVF(x, y, (res)))

#define int_sqrt(x)		((x) > 0 ? (int)sqrt((double)(x)) : 0)
#define int_sqrt32(x)		int_sqrt(x)
#define int_sqrt64(x)		((x) > 0 ? (int)sqrt((double)(x)) : 0)

#define check_zero_base(base)	((base) == 0)

/* Exclusive-end range validation, matching the pinned Linux contract. */
#define range_overflows(start, size, max) ({ \
	typeof(start) start__ = (start); \
	typeof(size) size__ = (size); \
	typeof(max) max__ = (max); \
	(void)(&start__ == &size__); \
	(void)(&start__ == &max__); \
	start__ >= max__ || size__ > max__ - start__; \
})
#define range_overflows_t(type, start, size, max) \
	range_overflows((type)(start), (type)(size), (type)(max))

static __always_inline int mul_u32_u32_to_u64(u32 a, u32 b, u64 *out)
{
	*out = (u64)a * b;
	return 0;
}

static __always_inline int mul_u64_u32_to_u64(u64 a, u32 b, u64 *out)
{
	return __builtin_mul_overflow((unsigned long long)a, (unsigned long long)b, (unsigned long long *)out);
}

static __always_inline int mul_u64_u64_to_u64(u64 a, u64 b, u64 *out)
{
	return __builtin_mul_overflow((unsigned long long)a, (unsigned long long)b, (unsigned long long *)out);
}

static __always_inline int div_u64_u32(u64 a, u32 b, u64 *out)
{
	if (b == 0)
		return -1;
	*out = a / b;
	return 0;
}

static __always_inline int mod_u64_u32(u64 a, u32 b, u64 *out)
{
	if (b == 0)
		return -1;
	*out = a % b;
	return 0;
}

static __always_inline int mod_u32_u32(u32 a, u32 b, u32 *out)
{
	if (b == 0)
		return -1;
	*out = a % b;
	return 0;
}

static __always_inline int div_u32_u32(u32 a, u32 b, u32 *out)
{
	if (b == 0)
		return -1;
	*out = a / b;
	return 0;
}

#define div64_u64(n, d)		((n) / (d))
#ifndef div_u64
#define div_u64(n, d)		((u64)(n) / (u32)(d))
#endif
#ifndef __div_u64
#define __div_u64(n, d)		((u64)(n) / (u32)(d))
#endif
#include <linux/math64.h>
#define __do_div_u64(n, d) do_div(n, d)

#endif /* __LINUX_OVERFLOW_H */
