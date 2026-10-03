#include <assert.h>
#include <time.h>
#include <linux/ktime.h>

extern u64 ktime_get_mono_fast_ns(void);

int main(void)
{
	ktime_t start = ktime_get();
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 12000000 };
	assert(nanosleep(&pause, NULL) == 0);
	ktime_t end = ktime_get();
	assert(end > start);
	assert(ktime_ms_delta(end, start) >= 10);
	assert(ktime_ms_delta(start, end) <= -10);
	assert(ktime_get_mono_fast_ns() > 0);
	assert(ktime_get_raw_ns() > 0);
	assert(ktime_get_boottime_ns() > 0);
	assert(ktime_get_real_seconds() > 1577836800);
	struct timespec64 timestamp = {0};
	ktime_get_ts64(&timestamp);
	assert(timestamp.tv_sec > 0 && timestamp.tv_nsec >= 0 &&
	       timestamp.tv_nsec < NSEC_PER_SEC);
	return 0;
}
