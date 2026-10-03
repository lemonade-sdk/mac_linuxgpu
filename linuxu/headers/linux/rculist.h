/* linuxu: SHIM (third_party/linux/include/linux/rculist.h) */
#ifndef _LINUX_RCU_LIST_H
#define _LINUX_RCU_LIST_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/compiler.h>

#define rcu_dereference(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_dereference_check(p, c) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_dereference_rhp(p, c) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)

#define rcu_assign_pointer(p, v) \
	do { __atomic_store_n(&(p), (v), __ATOMIC_RELEASE); } while (0)
#define rcu_assign_pointer_rhp(p, v, c) rcu_assign_pointer(p, v)
#define rcu_assign_pointer_debug(p, v, c) rcu_assign_pointer(p, v)

static inline void __list_add_rcu(struct list_head *new,
				  struct list_head *prev,
				  struct list_head *next)
{
	new->next = next;
	new->prev = prev;
	rcu_assign_pointer(next->prev, new);
	rcu_assign_pointer(prev->next, new);
}

static inline void list_add_rcu(struct list_head *new, struct list_head *head)
{
	__list_add_rcu(new, head, head->next);
}

static inline void list_add_tail_rcu(struct list_head *new, struct list_head *head)
{
	__list_add_rcu(new, head->prev, head);
}

static inline void list_add_force_rcu(struct list_head *new, struct list_head *head)
{
	__list_add_rcu(new, head, head->next);
}

static inline void list_del_rcu(struct list_head *entry)
{
	struct list_head *next = entry->next;
	struct list_head *prev = entry->prev;
	next->prev = prev;
	rcu_assign_pointer(prev->next, next);
	entry->prev = NULL;
}

static inline void list_del_rcu_bh(struct list_head *entry)
{
	list_del_rcu(entry);
}

static inline bool list_empty_rcu(const struct list_head *head)
{
	return list_empty(head);
}

static inline bool list_is_last_rcu(const struct list_head *list,
				     const struct list_head *head)
{
	return list_is_last(list, head);
}

#define rcu_access_pointer(p) READ_ONCE(p)
#define rcu_pointer_handoff(p) (p)

#define list_first_entry_or_null_rcu(head, type, member) ({ \
	struct list_head *__head = (head); \
	struct list_head *__first = rcu_dereference(__head->next); \
	__first != __head ? list_entry(__first, type, member) : NULL; \
})
#define list_for_each_entry_rcu(pos, head, member) \
	for ((pos) = list_first_entry_rcu(head, typeof(*(pos)), member); \
	     !list_entry_is_head(pos, head, member); \
	     (pos) = list_next_entry_rcu(pos, member))
#define list_first_entry_rcu(head, type, member) \
	list_entry(rcu_dereference((head)->next), type, member)
#define list_next_entry_rcu(pos, member) \
	list_entry(rcu_dereference(__list_cursor_head(pos, member)->next), typeof(*(pos)), member)
#define list_prev_entry_rcu(pos, type, member) \
	list_prev_entry(pos, type, member)
#define list_last_entry_rcu(head, type, member) \
	list_last_entry(head, type, member)
#define for_each_entry_rcu(pos, head, member) \
	for_each_entry(pos, head, member)
#define for_each_entry_continue_rcu(pos, head, member) \
	for_each_entry_continue(pos, head, member)
#define for_each_entry_safe_rcu(pos, n, head, member) \
	for_each_entry_safe(pos, n, head, member)

static inline void hlist_del_init_rcu(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		struct hlist_node *next = n->next;
		struct hlist_node **previous = n->pprev;
		rcu_assign_pointer(*previous, next);
		if (next) WRITE_ONCE(next->pprev, previous);
		/* Readers already at n must still be able to reach its successor. */
		WRITE_ONCE(n->pprev, NULL);
	}
}

static inline void hlist_add_head_rcu(struct hlist_node *n,
				       struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	n->pprev = &h->first;
	if (first) WRITE_ONCE(first->pprev, &n->next);
	rcu_assign_pointer(h->first, n);
}

#define hlist_for_each_entry_rcu(pos, head, member) \
	for ((pos) = hlist_entry_safe(rcu_dereference((head)->first), typeof(*(pos)), member); \
	     (pos); \
	     (pos) = hlist_entry_safe(rcu_dereference((pos)->member.next), typeof(*(pos)), member))

#endif /* _LINUX_RCU_LIST_H */
