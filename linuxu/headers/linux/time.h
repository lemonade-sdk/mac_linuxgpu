/* linuxu: AS-IS (third_party/linux/include/linux/time.h) */
#ifndef _LINUX_TIME_H
#define _LINUX_TIME_H

#include <linux/types.h>

/* uapi kernel types */
#ifndef __kernel_s64
#define __kernel_s64 s64
#endif
#ifndef __kernel_loff_t
#define __kernel_loff_t loff_t
#endif
#include <linux/const.h>

struct __kernel_timeval {
	__kernel_s64		tv_sec;
	__kernel_suseconds_t	tv_usec;
};

struct __kernel_timespec {
	__kernel_time64_t	tv_sec;
	long		tv_nsec;
};

struct old_timeval {
	long		tv_sec;
	long		tv_usec;
};

struct old_timeval32 {
	__kernel_old_time64_t	tv_sec;
	__kernel_old_suseconds_t	tv_usec;
};

struct old_timespec32 {
	__kernel_old_time64_t	tv_sec;
	__kernel_old_suseconds_t	tv_nsec;
};

struct __kernel_old_timeval {
	__kernel_old_time64_t	tv_sec;
	__kernel_old_suseconds_t	tv_usec;
};

struct __kernel_old_timespec {
	__kernel_old_time64_t	tv_sec;
	__kernel_old_suseconds_t	tv_nsec;
};

#define NSEC_PER_SEC	1000000000L
#define NSEC_PER_MSEC	1000000L
#define NSEC_PER_USEC	1000L

#define USEC_PER_SEC	1000000L
#define USEC_PER_MSEC	1000L

#define MSEC_PER_SEC	1000L

#define SEC_PER_DAY	86400L
#define SEC_PER_HOUR	3600L
#define SEC_PER_MIN	60L

#define NSEC_PER_DAY	(SEC_PER_DAY * NSEC_PER_SEC)
#define NSEC_PER_HOUR	(SEC_PER_HOUR * NSEC_PER_SEC)
#define NSEC_PER_MIN	(SEC_PER_MIN * NSEC_PER_SEC)

#define USEC_PER_DAY	(SEC_PER_DAY * USEC_PER_SEC)
#define USEC_PER_HOUR	(SEC_PER_HOUR * USEC_PER_SEC)
#define USEC_PER_MIN	(SEC_PER_MIN * USEC_PER_SEC)

#define MSEC_PER_DAY	(SEC_PER_DAY * MSEC_PER_SEC)
#define MSEC_PER_HOUR	(SEC_PER_HOUR * MSEC_PER_SEC)
#define MSEC_PER_MIN	(SEC_PER_MIN * MSEC_PER_SEC)


/* struct timespec64 */
#ifndef __LINUXU_TIMESPEC64_DEFINED
#define __LINUXU_TIMESPEC64_DEFINED
struct timespec64 {
	__kernel_s64	tv_sec;
	__kernel_loff_t	tv_nsec;
};
#endif

#ifndef time64_t
typedef __kernel_s64 time64_t;
#endif

/*
 * struct tm - kernel copy of the userspace struct (vendor 2026 time.h).
 * amdgpu_cper.c: time64_to_tm() for the CPER timestamp field.
 *
 * The host libc <time.h> already provides struct tm with the same
 * leading fields; on that platform do not re-define it (the kernel
 * definition is used only when no libc time.h is visible).
 */
#ifndef _LINUXU_TIME_TM_DEFINED
#define _LINUXU_TIME_TM_DEFINED
#if !defined(_TIME_H_) && !defined(_BSD_TIME_H) && !defined(_ANSI_TIME_H)
struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	long tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
	long tm_gmtoff;
	const char *tm_zone;
};
#endif /* _TIME_H */
#endif /* _LINUXU_TIME_TM_DEFINED */

/* Conversion helpers */
static inline struct timespec64 ns_to_timespec64(s64 ns)
{
	struct timespec64 ts;
	ts.tv_sec = ns / NSEC_PER_SEC;
	ts.tv_nsec = ns % NSEC_PER_SEC;
	if (ts.tv_nsec < 0) {
		ts.tv_sec--;
		ts.tv_nsec += NSEC_PER_SEC;
	}
	return ts;
}

static inline s64 timespec64_to_ns(const struct timespec64 *ts)
{
	return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

extern void time64_to_tm(time64_t totalsecs, int offset, struct tm *result);

#endif /* _LINUX_TIME_H */
