/* Linux timekeeping clock domains over the host/DriverKit clock adapter.
 * Uptime excludes system sleep; boottime includes it; realtime is wall time. */
#include <time.h>
#include <stdint.h>
#include <linux/ktime.h>
#include <rt/fatal.h>

static struct timespec read_clock(clockid_t clock_id)
{
	struct timespec value;
	if (clock_gettime(clock_id, &value) != 0)
		LINUXU_FATAL("timekeeping: clock_gettime failed");
	return value;
}

static u64 clock_ns(clockid_t clock_id)
{
	struct timespec value = read_clock(clock_id);
	return (u64)value.tv_sec * NSEC_PER_SEC + (u64)value.tv_nsec;
}

static void clock_ts64(clockid_t clock_id, struct timespec64 *value)
{
	struct timespec now = read_clock(clock_id);
	value->tv_sec = now.tv_sec;
	value->tv_nsec = now.tv_nsec;
}

ktime_t ktime_get(void) { return (ktime_t)clock_ns(CLOCK_UPTIME_RAW); }
ktime_t ktime_get_clock_monotonic(void) { return ktime_get(); }
u64 ktime_get_ns(void) { return clock_ns(CLOCK_UPTIME_RAW); }
u64 ktime_get_mono_fast_ns(void) { return ktime_get_ns(); }
ktime_t ktime_get_coarse(void) { return ktime_get(); }
u64 ktime_get_coarse_ns(void) { return ktime_get_ns(); }

ktime_t ktime_get_boottime(void) { return (ktime_t)clock_ns(CLOCK_MONOTONIC_RAW); }
u64 ktime_get_boottime_ns(void) { return clock_ns(CLOCK_MONOTONIC_RAW); }
ktime_t ktime_get_coarse_boottime(void) { return ktime_get_boottime(); }
u64 ktime_get_coarse_boottime_ns(void) { return ktime_get_boottime_ns(); }

u64 ktime_get_raw_ns(void) { return clock_ns(CLOCK_UPTIME_RAW); }
ktime_t ktime_get_real(void) { return (ktime_t)clock_ns(CLOCK_REALTIME); }
time64_t ktime_get_real_seconds(void) { return read_clock(CLOCK_REALTIME).tv_sec; }

void ktime_get_ts64(struct timespec64 *value)
{
	if (value) clock_ts64(CLOCK_UPTIME_RAW, value);
}

void ktime_get_raw_ts64(struct timespec64 *value)
{
	if (value) clock_ts64(CLOCK_UPTIME_RAW, value);
}

void ktime_get_real_ts64(struct timespec64 *value)
{
	if (value) clock_ts64(CLOCK_REALTIME, value);
}

void ktime_get_system_time(struct timespec *value)
{
	if (value) *value = read_clock(CLOCK_UPTIME_RAW);
}

void ktime_get_boottime_system(struct timespec *value)
{
	if (value) *value = read_clock(CLOCK_MONOTONIC_RAW);
}
