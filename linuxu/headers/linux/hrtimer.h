/* linuxu: EDITED (third_party/linux/include/linux/hrtimer.h) - timerqueue /
 * hrtimer_defs / hrtimer_types sub-headers inlined and flattened into a
 * shim-local struct hrtimer (base/cb_get_time handled by linuxu/src);
 * CONFIG_HIGH_RES_TIMERS and other kernel-only branches removed;
 * exported API kept. */
// SPDX-License-Identifier: GPL-2.0
#ifndef _LINUX_HRTIMER_H
#define _LINUX_HRTIMER_H

#include <linux/ktime.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/types.h>
#include <linux/err.h>

/* Mode arguments of hrtimer_start etc. (upstream hrtimer.h) */
enum hrtimer_mode {
	HRTIMER_MODE_ABS	= 0x00,
	HRTIMER_MODE_REL	= 0x01,
	HRTIMER_MODE_PINNED	= 0x02,
	HRTIMER_MODE_SOFT	= 0x04,
	HRTIMER_MODE_HARD	= 0x08,
	HRTIMER_MODE_LAZY_REARM	= 0x10,

	HRTIMER_MODE_ABS_PINNED = HRTIMER_MODE_ABS | HRTIMER_MODE_PINNED,
	HRTIMER_MODE_REL_PINNED = HRTIMER_MODE_REL | HRTIMER_MODE_PINNED,

	HRTIMER_MODE_ABS_SOFT	= HRTIMER_MODE_ABS | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_REL_SOFT	= HRTIMER_MODE_REL | HRTIMER_MODE_SOFT,

	HRTIMER_MODE_ABS_PINNED_SOFT = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_SOFT,
	HRTIMER_MODE_REL_PINNED_SOFT = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_SOFT,

	HRTIMER_MODE_ABS_HARD	= HRTIMER_MODE_ABS | HRTIMER_MODE_HARD,
	HRTIMER_MODE_REL_HARD	= HRTIMER_MODE_REL | HRTIMER_MODE_HARD,

	HRTIMER_MODE_ABS_PINNED_HARD = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_HARD,
	HRTIMER_MODE_REL_PINNED_HARD = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_HARD,
};

/* Return values for the callback function (upstream hrtimer_types.h) */
enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART,	/* Timer must be restarted */
};

struct hrtimer;
struct hrtimer_clock_base;

/*
 * struct hrtimer - the basic hrtimer structure (shim layout; linuxu/src
 * owns the real implementation and may extend this)
 */
/* vendor 2026 hrtimer.h layout: the expiry time lives in node.expires */
struct hrtimer_node {
	ktime_t expires;
};

struct hrtimer {
	struct hrtimer_node	node;	/* rb node of expiry (node.expires) */
	struct hrtimer_clock_base	*base;
	bool				is_queued;
	bool				is_rel;
	bool				is_soft;
	bool				is_hard;
	bool				is_lazy;
	ktime_t				_softexpires;
	enum hrtimer_restart		(*function)(struct hrtimer *);
};

struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};

static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = time;
	timer->_softexpires = time;
}

static inline void hrtimer_set_expires_range(struct hrtimer *timer, ktime_t time, ktime_t delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, delta);
}

static inline void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time, u64 delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, ns_to_ktime(delta));
}

static inline void hrtimer_add_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = ktime_add_safe(timer->node.expires, time);
	timer->_softexpires = ktime_add_safe(timer->_softexpires, time);
}

static inline void hrtimer_add_expires_ns(struct hrtimer *timer, u64 ns)
{
	timer->node.expires = ktime_add_ns(timer->node.expires, ns);
	timer->_softexpires = ktime_add_ns(timer->_softexpires, ns);
}

static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
	return timer->node.expires;
}

static inline ktime_t hrtimer_get_softexpires(const struct hrtimer *timer)
{
	return timer->_softexpires;
}

ktime_t hrtimer_cb_get_time(const struct hrtimer *timer);

static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *timer)
{
	return ktime_sub(timer->node.expires, hrtimer_cb_get_time(timer));
}

static inline ktime_t
__hrtimer_expires_remaining_adjusted(const struct hrtimer *timer, ktime_t now)
{
	return ktime_sub(timer->node.expires, now);
}

static inline ktime_t
hrtimer_expires_remaining_adjusted(const struct hrtimer *timer)
{
	return __hrtimer_expires_remaining_adjusted(timer, hrtimer_cb_get_time(timer));
}

static inline enum hrtimer_restart hrtimer_dummy_timeout(struct hrtimer *unused)
{
	return HRTIMER_NORESTART;
}

/* Exported timer functions: */

/* Initialize timers: */
extern void hrtimer_setup(struct hrtimer *timer, enum hrtimer_restart (*function)(struct hrtimer *),
			  clockid_t clock_id, enum hrtimer_mode mode);
extern void hrtimer_setup_on_stack(struct hrtimer *timer,
				   enum hrtimer_restart (*function)(struct hrtimer *),
				   clockid_t clock_id, enum hrtimer_mode mode);
extern void hrtimer_setup_sleeper_on_stack(struct hrtimer_sleeper *sl, clockid_t clock_id,
					   enum hrtimer_mode mode);

/* Basic timer operations: */
extern void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
				   u64 range_ns, const enum hrtimer_mode mode);

/**
 * hrtimer_start - (re)start an hrtimer
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @mode:	timer mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL), and pinned (HRTIMER_MODE_PINNED)
 */
static inline void hrtimer_start(struct hrtimer *timer, ktime_t tim,
				 const enum hrtimer_mode mode)
{
	hrtimer_start_range_ns(timer, tim, 0, mode);
}

extern int hrtimer_cancel(struct hrtimer *timer);
extern int hrtimer_try_to_cancel(struct hrtimer *timer);

static inline void hrtimer_start_expires(struct hrtimer *timer,
					 enum hrtimer_mode mode)
{
	u64 delta;
	ktime_t soft, hard;
	soft = hrtimer_get_softexpires(timer);
	hard = hrtimer_get_expires(timer);
	delta = ktime_to_ns(ktime_sub(hard, soft));
	hrtimer_start_range_ns(timer, soft, delta, mode);
}

void hrtimer_sleeper_start_expires(struct hrtimer_sleeper *sl,
				   enum hrtimer_mode mode);

static inline void hrtimer_restart(struct hrtimer *timer)
{
	hrtimer_start_expires(timer, HRTIMER_MODE_ABS);
}

/* Query timers: */
extern ktime_t __hrtimer_get_remaining(const struct hrtimer *timer, bool adjust);

static inline ktime_t hrtimer_get_remaining(const struct hrtimer *timer)
{
	return __hrtimer_get_remaining(timer, false);
}

extern bool hrtimer_active(const struct hrtimer *timer);

static inline bool hrtimer_is_queued(struct hrtimer *timer)
{
	return READ_ONCE(timer->is_queued);
}

/* Forward a hrtimer so it expires after now: */
extern u64
hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);

static inline u64 hrtimer_forward_now(struct hrtimer *timer,
				      ktime_t interval)
{
	return hrtimer_forward(timer, hrtimer_cb_get_time(timer), interval);
}

/* Precise sleep: */
extern int schedule_hrtimeout_range(ktime_t *expires, u64 delta,
				    const enum hrtimer_mode mode);
extern int schedule_hrtimeout_range_clock(ktime_t *expires,
					  u64 delta,
					  const enum hrtimer_mode mode,
					  clockid_t clock_id);
extern int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);
extern int schedule_hrtimeout_range_ns(ktime_t *expires, u64 delta_ns,
				       const enum hrtimer_mode mode);

#endif
