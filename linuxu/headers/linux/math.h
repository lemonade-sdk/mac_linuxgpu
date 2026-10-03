/* linuxu: SHIM (third_party/linux/include/linux/math.h) */
#ifndef _LINUX_MATH_H
#define _LINUX_MATH_H

#include <linux/types.h>
#include <uapi/linux/kernel.h>

/* 64-bit host: __udiv_qr8 collapses to a single 128-bit divide-free path */
#define __udiv_qr8(n_hi, n_lo, d, q_hi, q_lo, r) \
do { \
	unsigned long long __n = ((unsigned long long)(n_hi) << 32) | (n_lo); \
	unsigned long long __q = __n / (d); \
	*(q_hi) = (unsigned int)(__q >> 32); \
	*(q_lo) = (unsigned int)(__q); \
	*(r) = (unsigned int)(__n % (d)); \
} while (0)

#endif /* _LINUX_MATH_H */
