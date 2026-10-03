/* linuxu shim: delay — sleep helpers
 * (REAL: nanosleep-backed). */
#include <errno.h>
#include <stdint.h>
#include <time.h>

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>

void udelay(unsigned long us)
{
	nanosleep(&(struct timespec) {
			.tv_sec = us / 1000000UL,
			.tv_nsec = (us % 1000000UL) * 1000UL,
		}, NULL);
}

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
	while (nanosleep(&req, &rem) == -1 && errno == EINTR)
		req = rem;
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

void ssleep(unsigned int seconds)
{
	nanosleep(&(struct timespec) { .tv_sec = seconds }, NULL);
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
