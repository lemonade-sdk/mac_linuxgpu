/* linuxu shim: llist — lock-free singly linked list
 * (REAL minimal).  C11 atomics on the head pointer. */
#include <linux/llist.h>
#include <linux/atomic.h>

static bool head_cas(struct llist_head *head,
		     struct llist_node *old, struct llist_node *new)
{
	unsigned long *p = (unsigned long *)&head->first;
	unsigned long o = (unsigned long)old;

	return __atomic_compare_exchange_n(p, &o, (unsigned long)new,
					   true, __ATOMIC_ACQ_REL,
					   __ATOMIC_ACQUIRE);
}

bool llist_add(struct llist_node *new, struct llist_head *head)
{
	struct llist_node *old;

	do {
		old = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
		new->next = old;
	} while (!head_cas(head, old, new));
	return old == NULL;
}

bool llist_add_batch(struct llist_node *first, struct llist_node *last,
		     struct llist_head *head)
{
	struct llist_node *old;
	do {
		old = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
		last->next = old;
	} while (!head_cas(head, old, first));
	return old == NULL;
}

/* vendor 2026: returns the detached head node (caller walks/frees it) */
struct llist_node *llist_del_all(struct llist_head *head)
{
	struct llist_node *first;

	do {
		first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
		if (!first)
			return NULL;
	} while (!head_cas(head, first, NULL));
	return first;
}

struct llist_node *llist_del_first(struct llist_head *head)
{
	struct llist_node *first =
		__atomic_load_n(&head->first, __ATOMIC_ACQUIRE);

	while (first) {
		struct llist_node *next = first->next;

		if (head_cas(head, first, next))
			return first;
		first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
	}
	return NULL;
}

struct llist_node *llist_shift_all(struct llist_head *head)
{
	struct llist_node *first;

	do {
		first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
		if (!first)
			return NULL;
	} while (!head_cas(head, first, NULL));
	return first;
}

/* Removing an interior node needs exclusive ownership of the detached list.
 * This local helper can atomically remove only the current head. */
struct llist_node *llist_shift_node(struct llist_node *node,
				    struct llist_head *head)
{
	struct llist_node *first;
	do {
		first = __atomic_load_n(&head->first, __ATOMIC_ACQUIRE);
		if (!first || first != node) return NULL;
	} while (!head_cas(head, first, first->next));
	return first;
}
