/* linuxu: SHIM (third_party/linux/include/linux/notifier.h) */
#ifndef _LINUX_NOTIFIER_H
#define _LINUX_NOTIFIER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/compiler.h>

struct notifier_block {
	int (*notifier_call)(struct notifier_block *self,
			     unsigned long action, void *data);
	struct notifier_block __rcu *next;
	int priority;
};

typedef int (*notifier_fn_t)(unsigned long action, void *data);

/* notifier return values (upstream linux/notifier.h) */
#define NOTIFY_DONE		0x0000		/* Don't care */
#define NOTIFY_OK		0x0001		/* Suits me */
#define NOTIFY_STOP		0x0002		/* I want it to stop */

extern int register_notifier(struct notifier_block *nb, unsigned long action);
extern int unregister_notifier(struct notifier_block *nb, unsigned long action);
extern int register_raw_notifier(struct notifier_block *nb);
extern int unregister_raw_notifier(struct notifier_block *nb);
extern int notifier_call_chain(struct notifier_block **head,
			       unsigned long action, void *v);

static inline int blocking_notifier_call_chain(struct notifier_block **head,
					       unsigned long action, void *v)
{
	return notifier_call_chain(head, action, v);
}

struct atomic_notifier_head {
	struct notifier_block __rcu *head;
};

#define ATOMIC_NOTIFIER_HEAD(name) \
	struct atomic_notifier_head name = { .head = NULL }

#define DECLARE_ATOMIC_NOTIFIER_HEAD(name) \
	ATOMIC_NOTIFIER_HEAD(name)

static inline int atomic_notifier_register(struct atomic_notifier_head *nh,
					   struct notifier_block *nb)
{
	nb->next = nh->head;
	nh->head = nb;
	return 0;
}
static inline int atomic_notifier_unregister(struct atomic_notifier_head *nh,
					     struct notifier_block *nb)
{
	return 0;
}
static inline int atomic_notifier_call_chain(struct atomic_notifier_head *nh,
					     unsigned long action, void *v)
{
	return notifier_call_chain(&nh->head, action, v);
}

#endif /* _LINUX_NOTIFIER_H */
