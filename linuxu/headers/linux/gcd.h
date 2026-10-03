/* linuxu: SHIM (third_party/linux/include/linux/gcd.h) */
#ifndef __LINUX_GCD_H
#define __LINUX_GCD_H

#include <stdint.h>

static inline uint32_t __gcd32(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static inline uint64_t __gcd64(uint64_t a, uint64_t b)
{
	while (b) {
		uint64_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static inline uint32_t gcd(uint32_t a, uint32_t b)
{
	return __gcd32(a, b);
}

static inline uint64_t gcd64(uint64_t a, uint64_t b)
{
	return __gcd64(a, b);
}

static inline uint32_t lcm(uint32_t a, uint32_t b)
{
	if (!a || !b)
		return 0;
	return a / __gcd32(a, b) * b;
}

static inline uint64_t lcm64(uint64_t a, uint64_t b)
{
	if (!a || !b)
		return 0;
	return a / __gcd64(a, b) * b;
}

#endif /* __LINUX_GCD_H */
