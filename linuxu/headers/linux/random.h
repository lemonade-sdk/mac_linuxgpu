/* linuxu: SHIM (third_party/linux/include/linux/random.h)
 *
 * Kernel random numbers. Linux's get_random_*() return cryptographically
 * secure output from the kernel CRNG; here they come from the platform
 * CSPRNG (arc4random_buf(), also in DriverKit's libSystem) in
 * linuxu/src/shims/random.c.
 */
#ifndef _LINUX_RANDOM_H
#define _LINUX_RANDOM_H

#include <linux/types.h>
#include <linux/limits.h>

void get_random_bytes(void *buf, size_t len);
u8 get_random_u8(void);
u16 get_random_u16(void);
u32 get_random_u32(void);
u64 get_random_u64(void);

static inline unsigned long get_random_long(void)
{
	return (unsigned long)get_random_u64();
}

/* Uniform in [0, ceil). ceil must be non-zero, as in Linux. */
u32 __get_random_u32_below(u32 ceil);

static inline u32 get_random_u32_below(u32 ceil)
{
	return __get_random_u32_below(ceil);
}

/* Uniform in [floor, U32_MAX]. */
static inline u32 get_random_u32_above(u32 floor)
{
	return floor + 1 + get_random_u32_below(U32_MAX - floor);
}

/* Uniform in [floor, ceil]. */
static inline u32 get_random_u32_inclusive(u32 floor, u32 ceil)
{
	return floor + get_random_u32_below(ceil - floor + 1);
}

#endif /* _LINUX_RANDOM_H */
