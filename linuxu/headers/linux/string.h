/* linuxu: SHIM — maps to libc <string.h>, plus the small kernel surface
 * (word-sized block helpers + strscpy) the driver needs. */
#ifndef _LINUXU_LIBC_STRING_H
#define _LINUXU_LIBC_STRING_H
/* linux/bitops.h defines kernel macros that shadow libc prototypes
 * (int ffs(int), fls, ffsl, ffsll, flsl, flsll); keep them out of the
 * libc headers regardless of include order. */
#pragma push_macro("ffs")
#undef ffs
#pragma push_macro("fls")
#undef fls
#pragma push_macro("ffsl")
#undef ffsl
#pragma push_macro("ffsll")
#undef ffsll
#pragma push_macro("flsl")
#undef flsl
#pragma push_macro("flsll")
#undef flsll
#include <string.h>
#include <stdbool.h>
#pragma pop_macro("flsll")
#pragma pop_macro("flsl")
#pragma pop_macro("ffsll")
#pragma pop_macro("ffsl")
#pragma pop_macro("fls")
#pragma pop_macro("ffs")

/* ---- word-sized block helpers (upstream linux/io.h, folded in here so
 * every TU that has <linux/string.h> gets them; used by amdgpu_ring.h).
 * Plain C types on purpose: this header may be pulled in before types.h. ---- */
static inline void *memset32(volatile void *addr, unsigned int val, size_t count)
{
	volatile unsigned int *p = (volatile unsigned int *)addr;
	while (count--)
		*p++ = val;
	return (void *)addr;
}
static inline void *memset64(volatile void *addr, unsigned long val, size_t count)
{
	volatile unsigned long *p = (volatile unsigned long *)addr;
	while (count--)
		*p++ = val;
	return (void *)addr;
}
static inline void memcpy32(void *dst, const volatile void *src, size_t bytes)
{
	unsigned int *d = (unsigned int *)dst;
	const unsigned int *s = (const unsigned int *)src;
	size_t n = bytes / sizeof(unsigned int);
	while (n--)
		*d++ = *s++;
}
static inline void memcpy64(void *dst, const volatile void *src, size_t bytes)
{
	unsigned long *d = (unsigned long *)dst;
	const unsigned long *s = (const unsigned long *)src;
	size_t n = bytes / sizeof(unsigned long);
	while (n--)
		*d++ = *s++;
}

/* ---- kernel strscpy surface (vendor 2026 string.h, simplified).
 * Runtime: linuxu/src/kmem/string.c (sized_strscpy).
 *
 *   strscpy(dst, src)          -> sized_strscpy(dst, src, sizeof(dst))
 *   strscpy(dst, src, size)    -> sized_strscpy(dst, src, size)
 *
 * Dispatch: the GNU C sentinel trick (NARG_IMPL with N as the 4th
 * parameter; the variadic pack absorbs any extra args).
 */
ssize_t sized_strscpy(char *dst, const char *src, size_t size);
ssize_t sized_strscpy_pad(char *dst, const char *src, size_t size);

static inline ssize_t strscpy_3(char *dst, const char *src, size_t size)
{
	return sized_strscpy(dst, src, size);
}

#define __STRSCPY_2ARG(dst, src)         sized_strscpy((dst), (src), sizeof(dst))
#define __STRSCPY_3ARG(dst, src, size)   strscpy_3((dst), (src), (size))

/* Count total args (2 or 3) passed to strscpy.
 * GNU-style: the variadic pack is the 4th parameter of _IMPL, so
 * N lands on the right sentinel regardless of arg count. */
#define __STRSCPY_NARG_IMPL(_0, _1, _2, N, ...) N
#define __STRSCPY_NARG(...) __STRSCPY_NARG_IMPL(__VA_ARGS__, 3, 2, 1)
#define __STRSCPY_CONCAT_(a, b) a##b
#define __STRSCPY_CONCAT(a, b) __STRSCPY_CONCAT_(a, b)
#define __STRSCPY_ARG_2(dst, src)        __STRSCPY_2ARG(dst, src)
#define __STRSCPY_ARG_3(dst, src, size)  __STRSCPY_3ARG(dst, src, size)
#define strscpy(...) __STRSCPY_CONCAT(__STRSCPY_ARG_, __STRSCPY_NARG(__VA_ARGS__))(__VA_ARGS__)

/* kernel skip_spaces (vendor linux/string.h) */
static inline char *skip_spaces(const char *str)
{
	while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
		str++;
	return (char *)str;
}


static inline ssize_t strscpy_pad(char *dst, const char *src, size_t size)
{
	return sized_strscpy_pad(dst, src, size);
}

static inline long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	long result = 0;
	while (*cp == ' ' || *cp == '\t') cp++;
	int neg = 0;
	if (*cp == '-') { neg = 1; cp++; }
	else if (*cp == '+') cp++;
	if (base == 0) base = (*cp == '0') ? 8 : 10;
	while (*cp) {
		int d;
		if (*cp >= '0' && *cp < '0' + base) d = *cp - '0';
		else if (base > 10 && *cp >= 'a' && *cp < 'a' + base - 10) d = *cp - 'a' + 10;
		else if (base > 10 && *cp >= 'A' && *cp < 'A' + base - 10) d = *cp - 'A' + 10;
		else break;
		result = result * base + d;
		cp++;
	}
	if (neg) result = -result;
	if (endp) *endp = (char *)cp;
	return result;
}

static inline const char *strnchr(const char *s, size_t count, char c)
{
	const char *p = s;
	size_t i;
	for (i = 0; i < count; i++) {
		if (p[i] == c) return p + i;
		if (!p[i]) break;
	}
	return NULL;
}

static inline bool str_has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

#define strtomem_pad(dest, src, pad) do { \
	const char *__source = (src); \
	size_t __size = sizeof(dest), __length = strnlen(__source, __size); \
	memcpy((dest), __source, __length); \
	memset((dest) + __length, (pad), __size - __length); \
} while (0)
#endif /* _LINUXU_LIBC_STRING_H */
