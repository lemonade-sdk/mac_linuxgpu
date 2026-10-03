/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BUILD_BUG_H
#define _LINUX_BUILD_BUG_H

#include <linux/compiler.h>

/*
 * Force a compilation error if condition is true, but also produce a
 * result (of value 0 and type int), so the expression can be used
 * e.g. in a structure initializer (or where-ever else comma expressions
 * aren't permitted).
 *
 * Take an error message as an optional second argument. If omitted,
 * default to the stringification of the tested expression.
 */
#define __BUILD_BUG_ON_ZERO_MSG(e, msg) \
	(int)(1 ? (sizeof(char[1 - 2 * !!(e)])) : 0) * (int)0

#define BUILD_BUG_ON_ZERO(e, ...) \
	__BUILD_BUG_ON_ZERO_MSG(e, ##__VA_ARGS__, #e " is true")

/* Force a compilation error if a constant expression is not a power of 2 */
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)	\
	BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)			\
	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))

/*
 * BUILD_BUG_ON_INVALID() permits the compiler to check the validity of the
 * expression but avoids the generation of any code, even if that expression
 * has side-effects.
 */
#define BUILD_BUG_ON_INVALID(e) ((void)(sizeof((__force long)(e))))

#define __linuxu_compiletime_assert(cond, msg, id) do { \
	extern void id(void) __compiletime_error(msg); \
	if (!(cond)) id(); \
} while (0)
#define __linuxu_compiletime_assert_id(cond, msg, counter) \
	__linuxu_compiletime_assert(cond, msg, __linuxu_assert_ ## counter)
#define __linuxu_compiletime_assert_expand(cond, msg, counter) \
	__linuxu_compiletime_assert_id(cond, msg, counter)
#define compiletime_assert(cond, msg) \
	__linuxu_compiletime_assert_expand(cond, msg, __COUNTER__)

/**
 * BUILD_BUG_ON_MSG - break compile if a condition is true & emit supplied
 *		      error message.
 * @cond: the condition which the compiler should know is false.
 * @msg: build-time error message
 *
 * See BUILD_BUG_ON for description.
 */
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

/**
 * BUILD_BUG_ON - break compile if a condition is true.
 * @condition: the condition which the compiler should know is false.
 *
 * If you have some code which relies on certain constants being equal, or
 * some other compile-time-evaluated condition, you should use BUILD_BUG_ON to
 * detect if someone changes it.
 */
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)

/**
 * BUILD_BUG - break compile if used.
 *
 * If you have some code that you expect the compiler to eliminate at
 * build time, you should use BUILD_BUG to detect if it is
 * unexpectedly used.
 */
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")

/**
 * static_assert - check integer constant expression at build time
 * @expr: expression to be checked
 *
 * static_assert() is a wrapper for the C11 _Static_assert, with a
 * little macro magic to make the message optional (defaulting to the
 * stringification of the tested expression).
 *
 * Contrary to BUILD_BUG_ON(), static_assert() can be used at global
 * scope, but requires the expression to be an integer constant
 * expression (i.e., it is not enough that __builtin_constant_p() is
 * true for expr).
 *
 * Also note that BUILD_BUG_ON() fails the build if the condition is
 * true, while static_assert() fails the build if the expression is
 * false.
 */
#define static_assert(expr, ...) __static_assert(expr, ##__VA_ARGS__, #expr)
#define __static_assert(expr, msg, ...) _Static_assert(expr, msg)


/*
 * Compile time check that field has an expected offset
 */
#define ASSERT_STRUCT_OFFSET(type, field, expected_offset)	\
	BUILD_BUG_ON_MSG(offsetof(type, field) != (expected_offset),	\
		"Offset of " #field " in " #type " has changed.")



#ifndef offsetofend
#define offsetofend(TYPE, MEMBER) \
	(offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))
#endif

static inline size_t __linuxu_array_bytes(size_t count, size_t size)
{
	size_t result;
	return __builtin_mul_overflow(count, size, &result) ? (size_t)-1 : result;
}
static inline size_t __linuxu_struct_bytes(size_t base, size_t trailing)
{
	size_t result;
	return __builtin_add_overflow(base, trailing, &result) ? (size_t)-1 : result;
}
#define array_size(a, b) __linuxu_array_bytes((a), (b))
#define flex_array_size(p, member, count) \
	array_size((count), sizeof((p)->member[0]))
#define struct_size(p, member, count) \
	__linuxu_struct_bytes(sizeof(*(p)), flex_array_size(p, member, count))
#endif /* _LINUX_BUILD_BUG_H */
