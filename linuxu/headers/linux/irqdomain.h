/* linuxu: SHIM (third_party/linux/include/linux/irqdomain.h) - minimal IRQ
 * domain API surface used by the amdgpu IRQ path (linear domain create/
 * remove, alloc/free, irq_data access). Implementation in
 * linuxu/src/irq. */
/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQDOMAIN_H
#define _LINUX_IRQDOMAIN_H

#include <linux/types.h>
#include <linux/irq.h>
#include <linux/radix-tree.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

struct device_node;
struct msi_msg;
struct irq_data;

typedef unsigned int irq_hw_number_t;

struct irq_domain;

#define IRQ_DOMAIN_FLAG_NOMAP			BIT(0)
#define IRQ_DOMAIN_FLAG_HIERARCHY		BIT(1)
#define IRQ_DOMAIN_FLAG_NOPARENT		BIT(2)
#define IRQ_DOMAIN_FLAG_MSI_PARENT		BIT(3)
#define IRQ_DOMAIN_FLAG_MSI			BIT(4)
#define IRQ_DOMAIN_FLAG_NODIST			BIT(5)

struct irq_domain_ops {
	int	(*translate)(struct irq_domain *domain, struct irq_fwspec *fwspec,
			unsigned int *irq_number, irq_hw_number_t *hwirq);
	int	(*alloc)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs);
	int	(*alloc_at)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs, void *arg);
	void	(*free)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs);
	int	(*add_hw_irq)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs);
	void	(*remove_hw_irq)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs);
	void	(*select)(struct irq_domain *domain, unsigned int virq,
			unsigned int nr_irqs);
	int	(*map)(struct irq_domain *domain, unsigned int virq,
			irq_hw_number_t hwirq);
	void	(*unmap)(struct irq_domain *domain, unsigned int virq);
};

struct irq_domain {
	/* Private */
	spinlock_t		lock;
	const struct irq_domain_ops	*ops;
	struct radix_tree_root		radix;
	struct notifier_block		*nb;
	int			(*translate)(struct irq_domain *domain, struct irq_fwspec *fwspec,
				unsigned int *irq_number, irq_hw_number_t *hwirq);
	/* Public */
	unsigned int		fwnode_id;
	const char		*name;
	int			(*add_hw_irq)(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs);
	void			(*remove_hw_irq)(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs);
	void			(*select)(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs);
	unsigned int		flags;
};

extern struct irq_domain *irq_domain_create_linear(struct device_node *of_node,
						   unsigned int size,
						   const struct irq_domain_ops *ops,
						   void *arg);
extern void irq_domain_remove(struct irq_domain *d);
extern int irq_create_mapping(struct irq_domain *d, irq_hw_number_t hwirq);
extern int irq_find_mapping(struct irq_domain *domain, irq_hw_number_t hwirq);

#endif /* _LINUX_IRQDOMAIN_H */
