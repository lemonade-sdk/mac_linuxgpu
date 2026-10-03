/* linuxu: SHIM (third_party/linux/include/linux/llist.h) */
#ifndef _LINUX_LLIST_H
#define _LINUX_LLIST_H

#include <linux/atomic.h>
#include <linux/poison.h>
#include <linux/compiler.h>

struct llist_node {
	struct llist_node *next;
};

#define LLIST_HEAD_INIT(name) { .first = NULL }
#define LLIST_HEAD(name) \
	struct llist_head name = LLIST_HEAD_INIT(name)

#define LLIST_HEAD_INITIALIZER(name) LLIST_HEAD_INIT(name)

struct llist_head {
	struct llist_node *first;
};

struct llist_node;

static inline bool llist_empty(const struct llist_head *head)
{
	return READ_ONCE(head->first) == NULL;
}

static inline struct llist_node *llist_next(const struct llist_node *node)
{
	return node->next;
}

#define llist_entry(ptr, type, member) container_of(ptr, type, member)
#define llist_first_entry(ptr, type, member) ({ \
	struct llist_node *__node = (ptr); \
	__node ? llist_entry(__node, type, member) : NULL; })
#define llist_next_entry(pos, member) \
	llist_first_entry((pos)->member.next, typeof(*(pos)), member)

extern bool llist_add(struct llist_node *new, struct llist_head *head);
extern bool llist_add_batch(struct llist_node *first, struct llist_node *last,
			    struct llist_head *head);
extern struct llist_node *llist_del_all(struct llist_head *head);
extern struct llist_node *llist_del_first(struct llist_head *head);
extern struct llist_node *llist_shift_all(struct llist_head *head);
extern struct llist_node *llist_shift_node(struct llist_node *node,
					   struct llist_head *head);

static inline void init_llist_head(struct llist_head *head)
{
	head->first = NULL;
}
static inline void init_llist_node(struct llist_node *node)
{
	node->next = node;
}
static inline bool llist_on_list(const struct llist_node *node)
{
	return READ_ONCE(node->next) != node;
}
#define llist_for_each(pos, node) \
	for ((pos) = (node); (pos); (pos) = (pos)->next)
#define llist_for_each_safe(pos, n, node) \
	for ((pos) = (node); (pos) && (((n) = (pos)->next), true); (pos) = (n))
#define llist_for_each_entry(pos, head, member) \
	for ((pos) = llist_first_entry(head, typeof(*pos), member); \
	     (pos); (pos) = llist_next_entry(pos, member))
#define llist_for_each_entry_safe(pos, n, head, member) \
	for ((pos) = llist_first_entry(head, typeof(*pos), member); \
	     (pos) && (((n) = llist_next_entry(pos, member)), true); (pos) = (n))
#define for_each_llist_entry(pos, head, member) llist_for_each_entry(pos, head, member)
#endif /* _LINUX_LLIST_H */
