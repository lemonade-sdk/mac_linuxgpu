/* linuxu: SHIM (third_party/linux/include/linux/spinlock.h)
 *
 * spinlock/rwlock API surface. The shim maps every lock to a
 * pthread_mutex_t (see linuxu/src/sync/). The type definitions
 * follow the upstream layout closely enough for source compatibility.
 *
 * Runtime: linuxu/src/sync/spinlock.c
 */
#ifndef _LINUX_SPINLOCK_H
#define _LINUX_SPINLOCK_H

#include <linux/types.h>
#include <stdarg.h>
#include <linux/stringify.h>

/* ---- raw_spinlock (base type) ---- */
struct raw_spinlock {
	unsigned int owner;
	unsigned int count;
	unsigned int wait_lock;
};

typedef struct raw_spinlock raw_spinlock_t;

#define __ARCH_SPIN_LOCK_UNLOCKED { .owner = 0, .count = 1, .wait_lock = 0 }
#define __RAW_SPIN_LOCK_UNLOCKED	__ARCH_SPIN_LOCK_UNLOCKED
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED

#define _SPIN_LOCK_UNLOCKED	__RAW_SPIN_LOCK_UNLOCKED

/* ---- spinlock (wraps raw_spinlock, as non-RT upstream) ---- */
struct spinlock {
	union {
		struct raw_spinlock rlock;
	};
};

typedef struct spinlock spinlock_t;

#define ___SPIN_LOCK_INITIALIZER(lockname) { .rlock = _SPIN_LOCK_UNLOCKED }
#define __SPIN_LOCK_INITIALIZER(lockname)  { { .rlock = _SPIN_LOCK_UNLOCKED } }
#define __SPIN_LOCK_UNLOCKED(lockname)     (spinlock_t) __SPIN_LOCK_INITIALIZER(lockname)
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)

/* ---- rwlock: unified on struct rw_semaphore (see linux/rwlock.h) ---- */
#include <linux/rwsem.h>
typedef struct rw_semaphore rwlock_t;
#define __RW_LOCK_UNLOCKED(lockname) RWSEM_INITIALIZER(lockname)
#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_UNLOCKED(x)

/* ---- seqcount ---- */
struct seqcount {
	unsigned int seqcount;
};

/* ---- lock flags (irqsave) ---- */
typedef unsigned long irqflags_t;

/* ---- initializers (runtime: linuxu/src/sync/spinlock.c) ---- */
extern void raw_spin_lock_init(raw_spinlock_t *lock);
extern void spin_lock_init(spinlock_t *lock);
extern void rwlock_init(rwlock_t *lock);
extern void seqcount_init(struct seqcount *s);

/* ---- spinlock ops (runtime: linuxu/src/sync/spinlock.c) ---- */
extern void spin_lock(spinlock_t *lock);
extern void spin_unlock(spinlock_t *lock);
extern int   spin_trylock(spinlock_t *lock);
extern int   spin_trylock_irq(spinlock_t *lock);
extern void  spin_lock_irq(spinlock_t *lock);
extern void  spin_unlock_irq(spinlock_t *lock);
extern void  spin_lock_irqsave(spinlock_t *lock, irqflags_t flags);
extern void  spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags);
extern bool  spin_is_locked(spinlock_t *lock);
extern void  spin_lock_nested(spinlock_t *lock, int subclass);
extern bool  spin_trylock_nested(spinlock_t *lock, int subclass);
extern void  spin_lock_bh(spinlock_t *lock);
extern void  spin_unlock_bh(spinlock_t *lock);
extern bool  spin_trylock_bh(spinlock_t *lock);
extern void  spin_lock_irqsave_nobh(spinlock_t *lock, irqflags_t *flags);
extern void  spin_unlock_irqrestore_nobh(spinlock_t *lock, irqflags_t flags);

/* ---- raw_spinlock ops (runtime: linuxu/src/sync/spinlock.c) ---- */
extern void raw_spin_lock(raw_spinlock_t *lock);
extern void raw_spin_unlock(raw_spinlock_t *lock);
extern int  raw_spin_trylock(raw_spinlock_t *lock);
extern void raw_spin_lock_irq(raw_spinlock_t *lock);
extern void raw_spin_unlock_irq(raw_spinlock_t *lock);
extern void raw_spin_lock_irqsave(raw_spinlock_t *lock, irqflags_t flags);
extern void raw_spin_unlock_irqrestore(raw_spinlock_t *lock, irqflags_t flags);
extern bool raw_spin_is_locked(const raw_spinlock_t *lock);

/* The saved token is an output in Linux. Initialize it before entering the
 * shim, which has no CPU IRQ flags to save, instead of reading caller junk. */
#define spin_lock_irqsave(lock, flags) \
	((flags) = 0, (spin_lock_irqsave)((lock), 0))
#define raw_spin_lock_irqsave(lock, flags) \
	((flags) = 0, (raw_spin_lock_irqsave)((lock), 0))

/* ---- rwlock ops (runtime: linuxu/src/sync/rwlock.c) ---- */
extern void rwlock_read_lock(rwlock_t *lock);
extern void rwlock_read_unlock(rwlock_t *lock);
extern int  rwlock_read_trylock(rwlock_t *lock);
extern void rwlock_write_lock(rwlock_t *lock);
extern void rwlock_write_unlock(rwlock_t *lock);
extern int  rwlock_write_trylock(rwlock_t *lock);
extern void rwlock_read_lock_irqsave(rwlock_t *lock, irqflags_t flags);
extern void rwlock_read_unlock_irqrestore(rwlock_t *lock, irqflags_t flags);
extern void rwlock_write_lock_irqsave(rwlock_t *lock, irqflags_t *flags);
extern void rwlock_write_unlock_irqrestore(rwlock_t *lock, irqflags_t flags);

/* ---- seqlock ---- */
extern void seqcount_lock_begin(struct seqcount *s);
extern void seqcount_unlock(struct seqcount *s);
extern unsigned int seqcount_raw_read_begin(const struct seqcount *s);
extern bool seqcount_retry(const struct seqcount *s, unsigned int s1);

/* ---- irq flag helpers (shim: no-op, IRQs are always "enabled") ---- */
static inline void local_irq_restore(irqflags_t flags) { }
static inline void local_irq_disable(void) { }
static inline void local_irq_enable(void) { }
static inline bool local_irq_enabled(void) { return true; }
static inline void local_bh_disable(void) { }
static inline void local_bh_enable(void) { }
/* in_interrupt() — real, not a no-op.  The IRQ dispatch path (the dext
 * IOInterruptDispatchSource callback on the irqQueue) sets the linuxu
 * in_interrupt TLS flag around the IH-ring drain (spinlock.c:
 * linuxu_set_in_interrupt / in_irq).  The 6 KMD call sites
 * (amdgpu_gfx.c / amdgpu_gmc.c) rely on this being 1 during an IRQ so they
 * can skip a fence-wait / reschedule check (the IRQ handler
 * must never enter a blocking fence-wait).  On the host build the flag is a
 * __thread that is 0 unless a drain is in flight, so this is host-safe.  */
extern bool in_irq(void);
static inline bool in_interrupt(void) { return in_irq(); }
static inline bool in_atomic(void) { return false; }
static inline void might_fault(void) { }
static inline bool in_dbg_master(void) { return false; }
static inline bool irqs_disabled(void) { return false; }

/* ---- lockdep stubs ---- */
#define lockdep_is_held(lock)       true
#define lockdep_is_held_type(l,t)   true
#ifdef __LINUXU_NO_MIGHT_SLEEP
#define might_sleep()
#endif
#define might_lock()
#define might_lock_irq()
#define might_lock_irqsave()
#define might_lock_bh()
#define assert_spin_locked(l)
#define lockdep_assert_held(l)
#define lockdep_assert_held_once(l)
#define lockdep_assert_once(l)
#define lockdep_assert_held_read(l)
#define lockdep_assert_held_write(l)
#define lockdep_assert_not_held(l)
#define lockdep_assert_preemption_enabled()
#define lockdep_assert_preemption_disabled()
#define preempt_disable()
#define preempt_enable()
#define preempt_enable_notrace()
#define preempt_disable_notrace()
#define preempt_enable_no_resched()
#define preempt_disable_no_resched()
#define preempt_count() 0
#define preempt_count_add(n)
#define preempt_count_sub(n)
#define preempt_count_inc()
#define preempt_count_dec()
#define preempt_count_inc_notrace()
#define preempt_count_dec_notrace()
#define hardirq_count() 0
#define softirq_count() 0
#define irq_count() 0

#endif /* _LINUX_SPINLOCK_H */
