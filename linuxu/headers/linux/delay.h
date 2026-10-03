/* linuxu: EDITED (third_party/linux/include/linux/delay.h) - sleep/busy-wait
 * declarations kept; asm/delay.h dropped (busy loops implemented in
 * linuxu/src over nanosleep/spin); loops_per_jiffy kept as upstream
 * extern. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DELAY_H
#define _LINUX_DELAY_H

#include <linux/math.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>

extern unsigned long loops_per_jiffy;

/*
 * Copyright (C) 1993 Linus Torvalds
 *
 * Delay routines, using a pre-computed "loops_per_jiffy" value.
 * Sleep routines using timer list timers or hrtimers.
 */

extern void udelay(unsigned long usecs);
extern void udelay_range(unsigned long usecs_min, unsigned long usecs_max);
extern void ndelay(unsigned long nsecs);
extern void ndelay_range(unsigned long nsecs_min, unsigned long nsecs_max);
extern void mdelay(unsigned long msecs);
extern void mdelay_range(unsigned long msecs_min, unsigned long msecs_max);
void __ndelay(unsigned long);

/*
 * The following functions sleep for the amount of jiffies that
 * corresponds to the given number of milliseconds or usecs.
 *
 * The number of jiffies is rounded up to an even multiple of HZ.
 *
 * The returned value is 0 if the timer expired, or a negative value
 * if the call was interrupted.
 */
extern int msleep_interruptible(unsigned int msecs);
extern void msleep(unsigned int msecs);
extern void usleep_range_state(unsigned long min, unsigned long max,
			       unsigned int state);
extern void usleep_range(unsigned long min, unsigned long max);
extern bool wait_us_range(unsigned long min, unsigned long max);

/* Upstream inline helpers over the sleep primitives above. */
static inline void ssleep(unsigned int seconds)
{
	msleep(seconds * 1000);
}

static const unsigned int max_slack_shift = 2;
#define USLEEP_RANGE_UPPER_BOUND	((TICK_NSEC << max_slack_shift) / NSEC_PER_USEC)

/* fsleep - flexible sleep which autoselects the best mechanism (upstream). */
static inline void fsleep(unsigned long usecs)
{
	if (usecs <= 10)
		udelay(usecs);
	else if (usecs < USLEEP_RANGE_UPPER_BOUND)
		usleep_range(usecs, usecs + (usecs >> max_slack_shift));
	else
		msleep(DIV_ROUND_UP(usecs, USEC_PER_MSEC));
}

#endif
