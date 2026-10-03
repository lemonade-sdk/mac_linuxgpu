/* Capture native constants before Linux headers can accidentally redefine
 * them, then execute the production timekeeping adapter against a recorder. */
#include <assert.h>
#include <stdio.h>
#include <time.h>
enum {
    native_realtime = CLOCK_REALTIME,
    native_monotonic = CLOCK_MONOTONIC,
    native_boottime = CLOCK_MONOTONIC_RAW,
    native_uptime = CLOCK_UPTIME_RAW,
};
#include <linux/ktime.h>
#include <linux/bits.h>

static clockid_t selected_clock;
int linuxu_test_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    selected_clock = clock_id;
    value->tv_sec = 1000;
    value->tv_nsec = 17;
    return 0;
}
int main(void)
{
    assert(CLOCK_REALTIME == native_realtime);
    assert(CLOCK_MONOTONIC == native_monotonic);
    assert(CLOCK_MONOTONIC_RAW == native_boottime);
    assert(CLOCK_UPTIME_RAW == native_uptime);
    assert(ktime_get_ns() == 1000000000017ULL);
    assert(selected_clock == native_uptime);
    assert(ktime_get_raw_ns() == 1000000000017ULL);
    assert(selected_clock == native_uptime);
    assert(ktime_get_boottime_ns() == 1000000000017ULL);
    assert(selected_clock == native_boottime);
    assert(ktime_get_real_seconds() == 1000);
    assert(selected_clock == native_realtime);
    struct timespec64 value;
    ktime_get_ts64(&value);
    assert(selected_clock == native_uptime && value.tv_nsec == 17);
    ktime_get_real_ts64(&value);
    assert(selected_clock == native_realtime && value.tv_sec == 1000);
    assert(ktime_before(KTIME_MIN, KTIME_MAX));
    assert(ktime_after(KTIME_MAX, KTIME_MIN));
    assert(ktime_add_safe(KTIME_MAX - 1, 2) == KTIME_MAX);
    assert(ktime_add_safe(13, 29) == 42);
    assert(ktime_set(KTIME_SEC_MAX, 0) == KTIME_MAX);
    assert(timespec64_to_ktime((struct timespec64){ .tv_sec = KTIME_SEC_MAX }) == KTIME_MAX);
    value = (struct timespec64){ .tv_sec = 37, .tv_nsec = 42 };
    assert(!ktime_to_timespec64_cond(0, &value));
    assert(value.tv_sec == 37 && value.tv_nsec == 42);
    assert(ktime_to_timespec64_cond(-1, &value));
    assert(value.tv_sec == -1 && value.tv_nsec == NSEC_PER_SEC - 1);
    puts("Linux headers preserve native clock identifiers and timekeeping domains passed");
}
