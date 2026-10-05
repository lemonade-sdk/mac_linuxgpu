/* linuxu shim: delay — sleep helpers
 * (REAL: nanosleep-backed). */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>

#ifndef LINUXU_DEXT_DK
/* Below a millisecond the dext's nanosleep busy-waits (IODelay,
 * shims/dext_time.c), as Linux's udelay does. The host build waits the
 * same way: a sleeping nanosleep of a microsecond takes a timer slice
 * instead, so a register poll that times out (REG_WAIT, 100000 tries of
 * 1 us) took seconds here, and minutes on a virtual machine. */
static uint64_t delay_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Until @nsecs have passed both by the raw clock and by CLOCK_MONOTONIC as
 * clock_gettime reports it (microsecond steps on macOS), so a caller that
 * measures the delay with either sees at least @nsecs. */
static void delay_spin_ns(uint64_t nsecs)
{
	const uint64_t raw = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	const uint64_t mono = delay_monotonic_ns();

	while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - raw < nsecs ||
	       delay_monotonic_ns() - mono < nsecs)
		;
}
#endif

/* Busy-wait contract: return no earlier than @nsecs nanoseconds later.
 * nanosleep() never returns early except on EINTR, where the remainder is
 * slept again. */
void ndelay(unsigned long nsecs)
{
	struct timespec req = {
		.tv_sec = (time_t)(nsecs / 1000000000UL),
		.tv_nsec = (long)(nsecs % 1000000000UL),
	};
	struct timespec rem;

	if (!nsecs)
		return;
#ifndef LINUXU_DEXT_DK
	if (nsecs < 1000000UL) {
		delay_spin_ns(nsecs);
		return;
	}
#endif
	while (nanosleep(&req, &rem) == -1 && errno == EINTR)
		req = rem;
}

void udelay(unsigned long us)
{
	if (us > ULONG_MAX / 1000UL)
		us = ULONG_MAX / 1000UL;
	ndelay(us * 1000UL);
}

void mdelay(unsigned long ms)
{
	nanosleep(&(struct timespec) {
			.tv_sec = ms / 1000UL,
			.tv_nsec = (ms % 1000UL) * 1000000UL,
		}, NULL);
}

void msleep(unsigned int msecs)
{
	mdelay(msecs);
}

void usleep_range(unsigned long min, unsigned long max)
{
	(void)min;
	nanosleep(&(struct timespec) {
			.tv_sec = (time_t)(max / 1000000UL),
			.tv_nsec = (long)(max % 1000000UL) * 1000L,
		}, NULL);
}

void usleep_range_state(unsigned long min, unsigned long max,
			    unsigned int state)
{
	(void)state;
	usleep_range(min, max);
}

long schedule_timeout_uninterruptible(long timeout)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(timeout);
}
