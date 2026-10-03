/* linuxu: SHIM (third_party/linux/include/linux/rcupdate.h) */
#ifndef __LINUX_RCUPDATE_H
#define __LINUX_RCUPDATE_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/rculist.h>

#define RCU_LOCKDEP_WARN(lock, info)	do { } while (0)

extern void rcu_read_lock(void);
extern void rcu_read_unlock(void);
extern int rcu_read_lock_count(void);

/* Sleepable readers use independent grace periods for each SRCU domain. */
struct srcu_ctr {
	atomic_long_t srcu_locks;
	atomic_long_t srcu_unlocks;
};

struct srcu_struct {
	struct srcu_ctr *srcu_ctrp;
	unsigned long srcu_reader_flavor;
	u64 readers[2];
	unsigned int epoch;
	bool grace_period_active;
};

#define DEFINE_SRCU(name) \
	struct srcu_struct name = { .srcu_ctrp = NULL, .srcu_reader_flavor = 0 }
/* RCU pointer init (rcu_dereference / rcu_assign_pointer are macros in rculist.h) */
#define RCU_INIT_POINTER(p, v) do { (p) = (v); } while (0)
static inline void rcu_read_lock_bh(void) { rcu_read_lock(); }
static inline void rcu_read_unlock_bh(void) { rcu_read_unlock(); }
static inline void rcu_read_lock_sched(void) { rcu_read_lock(); }
static inline void rcu_read_unlock_sched(void) { rcu_read_unlock(); }
static inline void rcu_read_lock_irqsave(unsigned long flags) { (void)flags; rcu_read_lock(); }
static inline void rcu_read_unlock_irqrestore(unsigned long flags) { (void)flags; rcu_read_unlock(); }
static inline bool rcu_read_lock_sched_held(void) { return rcu_read_lock_count() > 0; }
static inline bool rcu_read_lock_bh_held(void) { return rcu_read_lock_count() > 0; }
static inline bool rcu_read_lock_any_held(void) { return rcu_read_lock_count() > 0; }
static inline bool rcu_read_lock_held(void) { return rcu_read_lock_count() > 0; }

/* grace-period / callback API */
extern void call_rcu(struct rcu_head *head,
		     void (*func)(struct rcu_head *head));
extern void synchronize_rcu(void);
extern void synchronize_rcu_expedited(void);
extern void synchronize_sched(void);
extern void synchronize_rcu_bh(void);
extern int srcu_read_lock(struct srcu_struct *ssp);
extern void srcu_read_unlock(struct srcu_struct *ssp, int idx);
extern void synchronize_srcu(struct srcu_struct *ssp);
extern void synchronize_srcu_expedited(struct srcu_struct *ssp);
extern void srcu_init_struct(struct srcu_struct *ssp);
extern int init_srcu_struct(struct srcu_struct *ssp);
extern void cleanup_srcu_struct(struct srcu_struct *ssp);
extern void srcu_init_observe(struct srcu_struct *ssp, int idx);
extern void srcu_read_lock_nohwpreempt(struct srcu_struct *ssp);
extern void srcu_read_unlock_nohwpreempt(struct srcu_struct *ssp, int idx);
/* Run @func after an SRCU grace period of @ssp, on the SRCU callback
 * worker; srcu_barrier waits for every callback queued before it. */
extern void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
		      void (*func)(struct rcu_head *head));
extern void srcu_barrier(struct srcu_struct *ssp);
extern void rcu_barrier(void);
extern void rcu_barrier_bh(void);
extern void rcu_barrier_sched(void);
extern int rcu_spawn_gp_kthread(void);


#define rcu_replace_pointer(p, new_value, take_lock) ({ \
	typeof(p) __old = rcu_dereference_protected(p, take_lock); \
	rcu_assign_pointer(p, new_value); \
	__old; \
})

#endif /* __LINUX_RCUPDATE_H */

#ifndef rcu_assign_pointer
#define rcu_assign_pointer(p, v) ((p) = (v))
#endif
