/* linuxu: SHIM (third_party/linux/include/linux/ratelimit.h + ratelimit_types.h)
 *
 * Aligned to the pinned vendor 2026 ratelimit_types.h layout so the
 * unmodified third_party/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_virt.c (DEFINE_RATELIMIT_STATE +
 * ratelimit_set_flags + RATELIMIT_MSG_ON_RELEASE) compiles.
 */
#ifndef _LINUX_RATELIMIT_H
#define _LINUX_RATELIMIT_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#ifndef HZ
#define HZ 1000	/* as linux/jiffies.h, which may include this first */
#endif

#define DEFAULT_RATELIMIT_INTERVAL	(5 * HZ)
#define DEFAULT_RATELIMIT_BURST		10

/* issue num suppressed message on exit */
#define RATELIMIT_MSG_ON_RELEASE	BIT(0)
#define RATELIMIT_INITIALIZED		BIT(1)

struct ratelimit_state {
	raw_spinlock_t	lock;		/* protect the state */

	int		interval;
	int		burst;
	atomic_t	rs_n_left;
	atomic_t	missed;
	unsigned int	flags;
	unsigned long	begin;
};

#define RATELIMIT_STATE_INIT_FLAGS(name, interval_init, burst_init, flags_init) { \
		.lock		= __RAW_SPIN_LOCK_UNLOCKED, \
		.interval	= interval_init, \
		.burst		= burst_init, \
		.flags		= flags_init, \
	}

#define RATELIMIT_STATE_INIT(name, interval_init, burst_init) \
	RATELIMIT_STATE_INIT_FLAGS(name, interval_init, burst_init, 0)

#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init)		\
									\
	struct ratelimit_state name =					\
		RATELIMIT_STATE_INIT(name, interval_init, burst_init)	\

static inline void ratelimit_state_init(struct ratelimit_state *rs,
					int interval, int burst)
{
	memset(rs, 0, sizeof(*rs));

	raw_spin_lock_init(&rs->lock);
	rs->interval	= interval;
	rs->burst	= burst;
}

static inline void ratelimit_default_init(struct ratelimit_state *rs)
{
	return ratelimit_state_init(rs, DEFAULT_RATELIMIT_INTERVAL,
					DEFAULT_RATELIMIT_BURST);
}

static inline void ratelimit_state_inc_miss(struct ratelimit_state *rs)
{
	atomic_inc(&rs->missed);
}

static inline int ratelimit_state_get_miss(struct ratelimit_state *rs)
{
	return atomic_read(&rs->missed);
}

static inline int ratelimit_state_reset_miss(struct ratelimit_state *rs)
{
	return atomic_xchg_relaxed(&rs->missed, 0);
}

static inline void ratelimit_state_reset_interval(struct ratelimit_state *rs, int interval_init)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rs->lock, flags);
	rs->interval = interval_init;
	rs->flags &= ~RATELIMIT_INITIALIZED;
	atomic_set(&rs->rs_n_left, rs->burst);
	ratelimit_state_reset_miss(rs);
	raw_spin_unlock_irqrestore(&rs->lock, flags);
}

static inline void ratelimit_state_exit(struct ratelimit_state *rs)
{
	int m;

	if (!(rs->flags & RATELIMIT_MSG_ON_RELEASE))
		return;

	m = ratelimit_state_reset_miss(rs);
	(void)m;
}

static inline void
ratelimit_set_flags(struct ratelimit_state *rs, unsigned long flags)
{
	rs->flags = flags;
}

extern struct ratelimit_state printk_ratelimit_state;

extern int ___ratelimit(struct ratelimit_state *rs, const char *func);
static inline bool __ratelimit(struct ratelimit_state *rs)
{
	return ___ratelimit(rs, __func__) != 0;
}
#define __ratelimit(state) ___ratelimit(state, __func__)

#endif /* _LINUX_RATELIMIT_H */
