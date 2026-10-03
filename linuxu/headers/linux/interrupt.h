/* linuxu: SHIM (third_party/linux/include/linux/interrupt.h)
 * IRQ handling surface: request_irq, free_irq, IRQF_*, etc.
 * Runtime: linuxu/src/sched/irq.c
 */
#ifndef __LINUX_INTERRUPT_H
#define __LINUX_INTERRUPT_H

#include <linux/types.h>
#include <linux/irqreturn.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>

typedef irqreturn_t (*irq_handler_t)(int, void *);

struct irqaction {
	irq_handler_t		handler;
	unsigned long		flags;
	const char		*name;
	void			*dev_id;
	struct irqaction	*next;
	int			irq;
	unsigned int		percpu_counter;
};

struct irq_chip;
struct irq_desc;
struct device;

#define IRQF_SHARED		0x00000020UL
#define IRQF_PERCPU		0x00000100UL
#define IRQF_ONESHOT		0x00004000UL
#define IRQF_PROBE_SHARED	0x00000080UL
#define IRQF_NO_THREAD		0x00040000UL
#define IRQF_EARLY		0x08000000UL
#define IRQF_NO_AUTOEN		0x10000000UL

extern int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
		       const char *dev_name, void *dev_id);
extern int request_threaded_irq(unsigned int irq, irq_handler_t handler,
				irq_handler_t thread_fn, unsigned long flags,
				const char *name, void *dev);
extern int devm_request_irq(struct device *d, unsigned int irq,
			    irq_handler_t handler, unsigned long flags,
			    const char *name, void *dev_id);
extern int devm_request_threaded_irq(struct device *d, unsigned int irq,
				     irq_handler_t handler, irq_handler_t thread_fn,
				     unsigned long flags, const char *name, void *dev);
extern void free_irq(unsigned int irq, void *dev_id);
extern void devm_free_irq(struct device *d, unsigned int irq, void *dev_id);
extern int disable_irq(unsigned int irq);
extern int disable_irq_nosync(unsigned int irq);
extern void enable_irq(unsigned int irq);
extern void enable_irq_nosync(unsigned int irq);
extern bool irq_disabled(void);
extern bool irq_enabled(void);
extern void synchronize_irq(unsigned int irq);
extern bool in_irq(void);
extern void local_irq_save(unsigned long flags);
extern void local_irq_restore(unsigned long flags);
extern void local_irq_disable(void);
extern void local_irq_enable(void);
extern void local_bh_disable(void);
extern void local_bh_enable(void);
extern void ack_irq(unsigned int irq);
extern void mask_irq(unsigned int irq);
extern void unmask_irq(unsigned int irq);
extern void __do_IRQ(unsigned int irq, struct pt_regs *regs);
extern void generic_handle_irq(unsigned int irq);
extern void generic_handle_domain_irq(struct irq_domain *d, unsigned int irq);
extern void generic_handle_arch_irq(unsigned int irq);

/* softirq */
enum softirq_action {
	HI_SOFTIRQ,
	TIMER_SOFTIRQ,
	NET_TX_SOFTIRQ,
	NET_RX_SOFTIRQ,
	BLOCK_SOFTIRQ,
	IRQ_SOFTIRQ,
	TASKLET_SOFTIRQ,
	SCHED_SOFTIRQ,
	HRTIMER_SOFTIRQ,
	RCU_SOFTIRQ,
	__MAX_SOFTIRQS
};

typedef void (*softirq_t)(int);

extern void open_softirq(int nr, softirq_t func);
extern void __do_softirq(void);
extern void do_softirq(void);
extern void smpboot_register_percpu_thread(void);
extern void smpboot_unregister_percpu_thread(void);

#endif /* __LINUX_INTERRUPT_H */
