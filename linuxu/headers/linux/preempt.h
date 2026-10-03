/* linuxu: SHIM (third_party/linux/include/linux/preempt.h) — no-op preemption */
#ifndef _LINUX_PREEMPT_H
#define _LINUX_PREEMPT_H

#include <linux/types.h>

#define preempt_disable()	do { } while (0)
#define preempt_enable()	do { } while (0)
#define preempt_enable_no_resched()	preempt_enable()
#define preempt_enable_resched()	preempt_enable()
#define preempt_disable_noraise()	preempt_disable()
#define preempt_enable_noresched()	preempt_enable()
#define preempt_enable_resched_notrace()	preempt_enable()
#define preempt_disable_notrace()	preempt_disable()
#define preempt_enable_notrace()	preempt_enable()
#define preempt_enable_no_resched_notrace()	preempt_enable()
#define preempt_enable_sched()	preempt_enable()
#define preempt_disable_sched()	preempt_disable()
#define preempt_enable_delay()
#define preempt_disable_delay()
#define preempt_enable_irq()
#define preempt_disable_irq()
#define preempt_count()		0
#define preempt_count_add(x)	do { } while (0)
#define preempt_count_sub(x)	do { } while (0)
#define preempt_count_inc()	do { } while (0)
#define preempt_count_dec()	do { } while (0)
#define might_sleep()		do { } while (0)
#define might_sleep_if(cond)	do { if (cond) might_sleep(); } while (0)
#define might_resched()		do { } while (0)
#define might_resched_rcu()	do { } while (0)
#define cond_resched()		false
#define cond_resched_rcu()	false
#define cond_resched_lock()	false
#define cond_resched_lock_irq()	false
#define cond_resched_softirq()	false
#define cond_resched_blocked()	false
#define cond_resched_blocked_lock()	false
#define cond_resched_maybe()	false
#define cond_resched_maybe_lock()	false
#define cond_resched_maybe_lock_irq()	false
#define cond_resched_maybe_lock_irqsave()	false
#define cond_resched_maybe_softirq()	false
#define cond_resched_maybe_softirq_noblock()	false
#define cond_resched_maybe_softirq_lock()	false
#define cond_resched_maybe_softirq_lock_irq()	false
#define cond_resched_maybe_softirq_lock_irqsave()	false
#define cond_resched_maybe_softirq_noblock_lock()	false
#define cond_resched_maybe_softirq_noblock_lock_irq()	false
#define cond_resched_maybe_softirq_noblock_lock_irqsave()	false
#define cond_resched_maybe_softirq_noblock_irqsave()	false
#define cond_resched_maybe_softirq_lock_noblock()	false
#define cond_resched_maybe_softirq_lock_noblock_irq()	false
#define cond_resched_maybe_softirq_lock_noblock_irqsave()	false

#define preempt_lazy_disable()	do { } while (0)
#define preempt_lazy_enable()	do { } while (0)
#define preempt_lazy_count()		0
#define preempt_lazy_count_add(x)	do { } while (0)
#define preempt_lazy_count_sub(x)	do { } while (0)
#define preempt_lazy_count_inc()	do { } while (0)
#define preempt_lazy_count_dec()	do { } while (0)


#endif /* _LINUX_PREEMPT_H */
