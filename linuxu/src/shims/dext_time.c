/* DriverKit timing for the Linux compatibility layer.  DriverKit.framework
 * provides the clock and sleep primitives used here; the dext does not link
 * macOS libSystem's POSIX clock_gettime or nanosleep implementations. */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_TEST_DEXT_TIME)

#include <stdint.h>
#include <time.h>
#include <errno.h>

#ifdef LINUXU_TEST_DEXT_TIME
#define clock_gettime linuxu_test_clock_gettime
#define nanosleep linuxu_test_nanosleep
#define clock_gettime_nsec_np linuxu_test_clock_ns
#define IOSleep linuxu_test_sleep_ms
#define IODelay linuxu_test_delay_us
#define __error linuxu_test_error
extern uint64_t linuxu_test_clock_ns(clockid_t clock_id);
extern int *linuxu_test_error(void);
#endif

/* DriverKit/IOLib.h declares these C functions. The KMD C compile rule uses
 * macOS C headers but no DriverKit framework header search path. */
extern void IOSleep(uint64_t milliseconds);
extern void IODelay(uint64_t microseconds);

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
	uint64_t ns;

	if (!tp) {
		errno = EINVAL;
		return -1;
	}
	switch (clock_id) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_UPTIME_RAW:
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	ns = clock_gettime_nsec_np(clock_id);
	if (!ns) {
		errno = EIO;
		return -1;
	}
	tp->tv_sec = (time_t)(ns / 1000000000ULL);
	tp->tv_nsec = (long)(ns % 1000000000ULL);
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	uint64_t milliseconds, microseconds;

	if (!req || req->tv_sec < 0 || req->tv_nsec < 0 ||
	    req->tv_nsec >= 1000000000L) {
		errno = EINVAL;
		return -1;
	}
	uint64_t fractional_ms = (uint64_t)req->tv_nsec / 1000000ULL;
	if ((uint64_t)req->tv_sec > (UINT64_MAX - fractional_ms) / 1000ULL) {
		errno = EOVERFLOW;
		return -1;
	}

	milliseconds = (uint64_t)req->tv_sec * 1000ULL +
		       fractional_ms;
	microseconds = ((uint64_t)req->tv_nsec % 1000000ULL + 999ULL) / 1000ULL;
	if (milliseconds)
		IOSleep(milliseconds);
	if (microseconds)
		IODelay(microseconds);
	if (rem) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}

#endif
