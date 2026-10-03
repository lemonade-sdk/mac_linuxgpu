/* linuxu: EDITED (third_party/linux/include/linux/math64.h; vdso/asm div64 inlined) */
#ifndef _LINUX_MATH64_H
#define _LINUX_MATH64_H

#include <linux/types.h>
#include <linux/math.h>

static inline u64 div64_ul(u64 n, unsigned long d) { return n / d; }

#define div64_u64(n, d)		((n) / (d))
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

/* Linux do_div returns a 32-bit remainder and updates the 64-bit dividend. */
#define do_div(n, base) \
	({ u32 __linuxu_base = (base); \
	   __typeof__(&(n)) __linuxu_n = &(n); \
	   u32 __linuxu_rem = (u64)*__linuxu_n % __linuxu_base; \
	   *__linuxu_n = (u64)*__linuxu_n / __linuxu_base; __linuxu_rem; })

#ifndef div_u64
#define div_u64(n, d) ((u64)(n) / (u32)(d))
#endif

#define div64_s64(n, d)		((s64)((n) / (d)))

#ifndef lower_32_bits
#define lower_32_bits(n)	((u32)((n) & 0xffffffffUL))
#endif

#ifndef div_u64_rem
static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	u64 tmp;

	tmp = dividend % divisor;
	*remainder = tmp;
	dividend /= divisor;
	return dividend;
}
#endif /* div_u64_rem */

/* 2026 overflow helpers used by drm core (mul_u32_u32 et al.; the
 * _to_u64 error forms live in linux/overflow.h, these are the scalar
 * return-value forms). */
#ifndef mul_u32_u32
static inline u64 mul_u32_u32(u32 a, u32 b)
{
	return (u64)a * b;
}
#endif
#ifndef add_u64_u32
static inline u64 add_u64_u32(u64 a, u32 b)
{
	return a + b;
}
#endif

#ifndef DIV_ROUND_CLOSEST
#define DIV_ROUND_CLOSEST(n, d)	(((n) + (d) / 2) / (d))
#endif
#ifndef DIV_ROUND_CLOSEST_ULL
#define DIV_ROUND_CLOSEST_ULL(ll, d) \
	({ u64 __n = (ll); u32 __d = (d); \
	   __n / __d + (__n % __d >= (u64)__d / 2 + (__d & 1)); })
#endif
#ifndef DIV_ROUND_DOWN_ULL
#define DIV_ROUND_DOWN_ULL(ll, d) ((u64)(ll) / (u32)(d))
#endif
#ifndef DIV_ROUND_UP_ULL
#define DIV_ROUND_UP_ULL(ll, d) \
	({ u64 __n = (ll); u32 __d = (d); __n / __d + !!(__n % __d); })
#endif


/* Upstream rounding forms. */
#ifndef DIV64_U64_ROUND_UP
#define DIV64_U64_ROUND_UP(ll, d)	\
	({ u64 _tmp = (d); div64_u64((ll) + _tmp - 1, _tmp); })
#define DIV_U64_ROUND_UP(ll, d)		\
	({ u32 _tmp = (d); div_u64((ll) + _tmp - 1, _tmp); })
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor)	\
	({ u64 _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })
#define DIV_U64_ROUND_CLOSEST(dividend, divisor)	\
	({ u32 _tmp = (divisor); div_u64((u64)(dividend) + _tmp / 2, _tmp); })
#define DIV_S64_ROUND_CLOSEST(dividend, divisor)(	\
{							\
	s64 __x = (dividend);				\
	s32 __d = (divisor);				\
	((__x > 0) == (__d > 0)) ?			\
		(s64)((__x + (__d / 2)) / __d) :	\
		(s64)((__x - (__d / 2)) / __d);		\
}							\
)
static inline u64 roundup_u64(u64 x, u32 y)
{
	return DIV_U64_ROUND_UP(x, y) * y;
}
#endif

#endif /* _LINUX_MATH64_H */
