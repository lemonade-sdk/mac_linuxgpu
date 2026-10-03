/* linuxu: SHIM (third_party/linux/include/linux/jiffies.h)
 *
 * Jiffies over a monotonic 100 Hz userspace counter.
 * Runtime: linuxu/src/sync/time.c (or kmem).
 */
#ifndef __LINUX_JIFFIES_H
#define __LINUX_JIFFIES_H

#include <linux/kernel.h>
#include <linux/const.h>
#include <linux/types.h>
#include <limits.h>
#include <time.h>
#include <rt/fatal.h>

/* 100 Hz tick (matches CONFIG_HZ=100 in the shim autoconf) */
#define HZ			100
#define USER_HZ			100
#define CONFIG_HZ			100

/* Read the clock on demand; no kernel timer interrupt updates a global. */
static inline unsigned long long get_jiffies_64(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_UPTIME_RAW, &now) != 0)
		LINUXU_FATAL("jiffies: uptime clock unavailable");
	return (unsigned long long)now.tv_sec * HZ +
	       (unsigned long long)now.tv_nsec / (1000000000ULL / HZ);
}

#define jiffies_64 (get_jiffies_64())
#define jiffies ((unsigned long)get_jiffies_64())

static inline unsigned long get_jiffies(void)
{
	return (unsigned long)jiffies_64;
}

#define jiffies_64_to_clock_t(x)	(((x) * (long)USER_HZ) / HZ)
#define clock_t_to_jiffies_64(x)	((x) * HZ / (long)USER_HZ)
#define jiffies_64_to_msecs(x)		((x) * 1000UL / HZ)
#define jiffies_to_msecs(x)		((x) * 1000UL / HZ)
#define jiffies_to_usecs(x)		((x) * 1000000UL / HZ)
#define msecs_to_jiffies64(x)		msecs_to_jiffies(x)
#define jiffies64_to_msecs(x)		((x) * 1000UL / HZ)

#define usecs_to_clock_t(x)		((x) * USER_HZ / 1000000UL)
#define jiffies_to_clock_t(x)		((x) * USER_HZ / HZ)
#define clock_t_to_jiffies(x)		((x) * HZ / USER_HZ)
#define clock_t_to_msecs(x)		((x) * 1000UL / USER_HZ)
#define msecs_to_clock_t(x)		((x) * USER_HZ / 1000UL)
#define clock_t_to_usecs(x)		((x) * 1000000UL / USER_HZ)

#ifndef MAX_JIFFY_OFFSET
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#endif

static inline unsigned long msecs_to_jiffies(unsigned int milliseconds)
{
	if ((int)milliseconds < 0)
		return MAX_JIFFY_OFFSET;
	return ((unsigned long)milliseconds + 1000 / HZ - 1) / (1000 / HZ);
}

static inline unsigned long usecs_to_jiffies(unsigned int microseconds)
{
	return ((unsigned long)microseconds + 1000000 / HZ - 1) / (1000000 / HZ);
}

/*
 * jiffies comparison helpers (kernel time.h).  The upstream 6.6+ kernel
 * guards these with CONFIG_HAVE_NVTIME / wraps them per-arch, but the
 * shim always has them (plain unsigned comparison of jiffies values).
 */
#ifndef time_after
#define time_after(a, b) ((long)((unsigned long)(b) - (unsigned long)(a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((unsigned long)(a) - (unsigned long)(b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#endif

#ifndef time_is_before_jiffies
/* jiffies-flavoured comparison (upstream linux/timekeeping.h) */
static inline bool time_is_before_jiffies(unsigned long x)
{
	return time_before(x, jiffies_64);
}

static inline bool time_is_after_jiffies(unsigned long x)
{
	return time_after(x, jiffies_64);
}
#endif

#endif /* __LINUX_JIFFIES_H */
#ifndef nsecs_to_jiffies
/* HZ divides one second exactly. Divide first so long deadlines do not
 * overflow before conversion (amdgpu_gem_timeout accepts an s64 duration). */
#define nsecs_to_jiffies(x) ((unsigned long)((u64)(x) / (1000000000ULL / HZ)))
#endif
#ifndef nsecs_to_jiffies64
#define nsecs_to_jiffies64(x) ((u64)(x) / (1000000000ULL / HZ))
#endif
