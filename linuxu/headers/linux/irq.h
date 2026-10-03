/* linuxu: SHIM (third_party/linux/include/linux/irq.h) - minimal IRQ API
 * surface used by the amdgpu IRQ path (request_irq/free_irq,
 * irqreturn_t, IRQF_* flags, struct irq_chip + handler prototypes).
 * The real interrupt machinery (IOInterruptDispatchSource) lives in
 * linuxu/src/irq. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQ_H
#define _LINUX_IRQ_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/irqreturn.h>
#include <linux/interrupt.h>

struct cpumask;
struct seq_file;
struct msi_msg;

/*
 * Flags used by request_irq() (subset of upstream values).
 */
#define IRQF_SHARED			0x00000020
#define IRQF_PROBE			0x00000040
#define IRQF_PERCPU			0x00000080
#define IRQF_NOPROBE			0x00000100
#define IRQF_NOTIFIER			0x00000200
#define IRQF_EFFECTIVE_AFF_MASK		0x00000400
#define IRQF_FORCE_RESORT		0x00000800
#define IRQF_COND_SUSPEND		0x00001000
#define IRQF_NO_AUTOENAB			0x00002000
#define IRQF_NO_BALANCE			0x00004000
#define IRQF_DISABLE			0x00008000

#define IRQF_SAMPLE_RANDOM		0x00010000
#define IRQF_EARLYBALANCE		0x00020000
#define IRQF_NESTED			0x00040000
#define IRQF_MOVEONLY			0x00080000
#define IRQF_RESET			0x00100000
#define IRQF_MANAGED			0x00200000
#define IRQF_COND_UNMASK		0x00400000
#define IRQF_IRQPOLL			0x00800000
#define IRQF_PERCPU_DEVID			0x01000000
#define IRQF_AFFINITY_MANAGED		0x02000000
#define IRQF_NO_THREAD			0x04000000
#define IRQF_AFFINITY_MANAGED		0x02000000
#define IRQF_TRIGGER_MASK		0x000000ff
#define IRQF_TRIGGER_NONE		0x00000000
#define IRQF_TRIGGER_RISE		0x00000001
#define IRQF_TRIGGER_FALL		0x00000002
#define IRQF_TRIGGER_HI		0x00000004
#define IRQF_TRIGGER_LO		0x00000008

struct irq_data;

/*
 * struct irq_chip - hardware interrupt chip descriptor (upstream layout,
 * CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE members removed)
 */
struct irq_chip {
	const char	*name;
	unsigned int	(*irq_startup)(struct irq_data *data);
	void		(*irq_shutdown)(struct irq_data *data);
	void		(*irq_enable)(struct irq_data *data);
	void		(*irq_disable)(struct irq_data *data);

	void		(*irq_ack)(struct irq_data *data);
	void		(*irq_mask)(struct irq_data *data);
	void		(*irq_mask_ack)(struct irq_data *data);
	void		(*irq_unmask)(struct irq_data *data);
	void		(*irq_eoi)(struct irq_data *data);

	int		(*irq_set_affinity)(struct irq_data *data, const struct cpumask *dest, bool force);
	void		(*irq_pre_redirect)(struct irq_data *data);
	int		(*irq_retrigger)(struct irq_data *data);
	int		(*irq_set_type)(struct irq_data *data, unsigned int flow_type);
	int		(*irq_set_wake)(struct irq_data *data, unsigned int on);

	void		(*irq_bus_lock)(struct irq_data *data);
	void		(*irq_bus_sync_unlock)(struct irq_data *data);

	void		(*irq_suspend)(struct irq_data *data);
	void		(*irq_resume)(struct irq_data *data);
	void		(*irq_pm_shutdown)(struct irq_data *data);

	void		(*irq_calc_mask)(struct irq_data *data);

	void		(*irq_print_chip)(struct irq_data *data, struct seq_file *p);
	int		(*irq_request_resources)(struct irq_data *data);
	void		(*irq_release_resources)(struct irq_data *data);

	void		(*irq_compose_msi_msg)(struct irq_data *data, struct msi_msg *msg);
	void		(*irq_write_msi_msg)(struct irq_data *data, struct msi_msg *msg);

	int		(*irq_get_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool *state);
	int		(*irq_set_irqchip_state)(struct irq_data *data, enum irqchip_irq_state which, bool state);

	int		(*irq_set_vcpu_affinity)(struct irq_data *data, void *vcpu_info);

	void		(*ipi_send_single)(struct irq_data *data, unsigned int cpu);
	void		(*ipi_send_mask)(struct irq_data *data, const struct cpumask *dest);

	int		(*irq_nmi_setup)(struct irq_data *data);
	void		(*irq_nmi_teardown)(struct irq_data *data);

	void		(*irq_force_complete_move)(struct irq_data *data);

	unsigned long	flags;
};

enum irqchip_irq_state {
	IRQCHIP_STATE_PENDING = (1 << 0),
	IRQCHIP_STATE_MASKED = (1 << 1),
	IRQCHIP_STATE_EOI = (1 << 2),
};

/*
 * irq handlers (upstream linux/irqhandler.h): the simple handler is what
 * amdgpu_irqdomain_map() installs.
 */
typedef irqreturn_t (*irq_handler_t)(int, void *);

extern irqreturn_t handle_simple_irq(int irq, void *dev_id);

extern int request_irq(unsigned int irq, irq_handler_t handler,
		       unsigned long flags, const char *name, void *dev);
extern void free_irq(unsigned int irq, void *dev_id);

extern void irq_set_chip_and_handler(int irq, struct irq_chip *chip,
				     irq_handler_t handler);

/* in_interrupt() is provided by linuxu sched.h; keep the name here too. */
extern bool in_interrupt(void);

#endif /* _LINUX_IRQ_H */
