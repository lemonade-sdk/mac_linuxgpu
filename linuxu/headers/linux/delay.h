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

#endif
