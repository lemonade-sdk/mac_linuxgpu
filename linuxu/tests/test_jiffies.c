#include <assert.h>
#include <stdio.h>
#define clock_gettime test_clock_gettime
#include <linux/jiffies.h>
#undef clock_gettime

static struct timespec fake_now;
int test_clock_gettime(clockid_t clock, struct timespec *out)
{
	assert(clock == CLOCK_UPTIME_RAW);
	*out = fake_now;
	return 0;
}
int main(void)
{
	/* HZ=1000: a jiffy is a millisecond. */
	assert(HZ == 1000);
	fake_now = (struct timespec){100, 999999};
	assert(jiffies == 100000);
	fake_now.tv_nsec = 1000000;
	assert(get_jiffies_64() == 100001);
	assert(msecs_to_jiffies(0) == 0);
	assert(msecs_to_jiffies(1) == 1);
	assert(msecs_to_jiffies(10) == 10);
	assert(msecs_to_jiffies(11) == 11);
	assert(msecs_to_jiffies(-1) == MAX_JIFFY_OFFSET);
	assert(usecs_to_jiffies(1) == 1);
	assert(usecs_to_jiffies(1001) == 2);
	assert(nsecs_to_jiffies(999999) == 0);
	assert(nsecs_to_jiffies(1000000) == 1);
	assert(nsecs_to_jiffies(LLONG_MAX) == (unsigned long long)LLONG_MAX / 1000000);
	assert(nsecs_to_jiffies64(ULLONG_MAX) == ULLONG_MAX / 1000000);
	u64 nanoseconds = 1000000;
	assert(nsecs_to_jiffies(nanoseconds++) == 1 && nanoseconds == 1000001);
	assert(jiffies_to_msecs(250) == 250);
	assert(time_after(2UL, ULONG_MAX - 2));
	assert(time_before(ULONG_MAX - 2, 2UL));
	assert(time_after_eq(2UL, 2UL) && time_before_eq(2UL, 2UL));
	assert(!time_after(2UL, 2UL));
	unsigned long deadline = jiffies + msecs_to_jiffies(20);
	assert(time_before(jiffies, deadline));
	fake_now.tv_nsec += 30000000;
	assert(time_after(jiffies, deadline));
	puts("jiffies progress, rounding, deadline, and rollover: PASS");
}
