/* linuxu: SHIM (third_party/linux/include/linux/ktime.h)
 * ktime_t is a plain s64 (nanoseconds). Conversion helpers inline.
 */
#ifndef _LINUX_KTIME_H
#define _LINUX_KTIME_H

#include <linux/types.h>
#include <linux/time.h>
#include <linux/limits.h>

typedef s64 ktime_t;

#define KTIME_MAX		((ktime_t)S64_MAX)
#define KTIME_MIN		((ktime_t)S64_MIN)
#define KTIME_SEC_MAX		((s64)(S64_MAX / NSEC_PER_SEC))
#define KTIME_SEC_MIN		((s64)(S64_MIN / NSEC_PER_SEC))

static inline ktime_t ktime_set(const s64 secs, const unsigned long nsecs)
{
	if (secs >= KTIME_SEC_MAX)
		return KTIME_MAX;
	return (ktime_t)secs * NSEC_PER_SEC + (ktime_t)nsecs;
}

/* convert a timespec64 to ktime_t format */
static inline ktime_t timespec64_to_ktime(const struct timespec64 ts)
{
	return ktime_set(ts.tv_sec, ts.tv_nsec);
}

#define ktime_to_timespec64(kt)		ns_to_timespec64(kt)

static inline s64 ktime_to_ns(const ktime_t kt)
{
	return (s64)kt;
}

static inline ktime_t ns_to_ktime(s64 nsec)
{
	return (ktime_t)nsec;
}

static inline s64 ktime_to_us(const ktime_t kt)
{
	return (s64)kt / NSEC_PER_USEC;
}

static inline ktime_t us_to_ktime(s64 usec)
{
	return (ktime_t)(usec * NSEC_PER_USEC);
}

static inline s64 ktime_to_ms(const ktime_t kt)
{
	return (s64)kt / NSEC_PER_MSEC;
}

static inline ktime_t ms_to_ktime(s64 msec)
{
	return (ktime_t)(msec * NSEC_PER_MSEC);
}

static inline s64 ktime_divns(const ktime_t div, s64 ns)
{
	return (s64)div / ns;
}

static inline ktime_t ktime_add(ktime_t a, ktime_t b)
{
	return (ktime_t)((s64)a + (s64)b);
}
static inline ktime_t ktime_add_safe(ktime_t a, ktime_t b)
{
	/* Positive timeout additions saturate instead of wrapping into an
	 * already-expired deadline. The unsigned sum avoids signed overflow. */
	ktime_t sum = (ktime_t)((u64)a + (u64)b);
	return sum < 0 || sum < a || sum < b ? KTIME_MAX : sum;
}
#ifndef ktime_add_ns
#define ktime_add_ns(t, ns)	(ktime_add((t), ns_to_ktime((ns))))
#endif

static inline ktime_t ktime_sub(ktime_t a, ktime_t b)
{
	return (ktime_t)((s64)a - (s64)b);
}

static inline bool ktime_before(ktime_t a, ktime_t b)
{
	return a < b;
}

static inline bool ktime_after(ktime_t a, ktime_t b)
{
	return a > b;
}

static inline int ktime_compare(ktime_t a, ktime_t b)
{
	if ((s64)a < (s64)b)
		return -1;
	if ((s64)a > (s64)b)
		return 1;
	return 0;
}

static inline bool ktime_to_timespec64_cond(const ktime_t kt, struct timespec64 *ts)
{
	if (!kt)
		return false;
	*ts = ktime_to_timespec64(kt);
	return true;
}

extern time64_t ktime_get_real_seconds(void);
extern void ktime_get_ts64(struct timespec64 *ts);
extern void ktime_get_real_ts64(struct timespec64 *tv);
extern void ktime_get_raw_ts64(struct timespec64 *ts);
extern ktime_t ktime_get(void);
extern ktime_t ktime_get_real(void);
extern ktime_t ktime_get_boottime(void);
extern ktime_t ktime_get_coarse(void);
extern ktime_t ktime_get_coarse_boottime(void);
extern u64 ktime_get_ns(void);
extern u64 ktime_get_raw_ns(void);
extern u64 ktime_get_coarse_ns(void);
extern ktime_t ktime_get_clock_monotonic(void);
extern void ktime_get_system_time(struct timespec *tv);
extern void ktime_get_boottime_system(struct timespec *tv);

static inline s64 ktime_ms_delta(ktime_t end, ktime_t start)
{
	return (end - start) / NSEC_PER_MSEC;
}

static inline s64 ktime_us_delta(const ktime_t later, const ktime_t earlier)
{
	return (later - earlier) / NSEC_PER_USEC;
}

static inline ktime_t ktime_add_us(const ktime_t kt, const u64 usec)
{
	return kt + (ktime_t)(usec * NSEC_PER_USEC);
}

static inline ktime_t ktime_sub_us(const ktime_t kt, const u64 usec)
{
	return kt - (ktime_t)(usec * NSEC_PER_USEC);
}

extern u64 ktime_get_boottime_ns(void);
extern u64 ktime_get_coarse_boottime_ns(void);


/* ktime_sub_ns — host shim: ktime_t is plain s64 */
static inline ktime_t ktime_sub_ns(ktime_t kt, u64 nsec)
{
	return (ktime_t)((long long)kt - (long long)nsec);
}
#endif /* _LINUX_KTIME_H */
