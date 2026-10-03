/* Single-node Linux LRU ownership for the upstream TTM page pools. */
#include <limits.h>
#include <linux/errno.h>
#include <linux/list_lru.h>

int list_lru_init(struct list_lru *lru)
{
	if (!lru) return -EINVAL;
	spin_lock_init(&lru->lock);
	lru->items_per_cpu = 0;
	lru->nr_items = 0;
	lru->node = &lru->local_node;
	INIT_LIST_HEAD(&lru->node->list);
	lru->node->nr_items = 0;
	lru->node->owner = lru;
	spin_lock_init(&lru->node->lock);
	return 0;
}

void list_lru_destroy(struct list_lru *lru)
{
	if (!lru || !lru->node) return;
	spin_lock(&lru->local_node.lock);
	/* The caller owns every queued object; destruction cannot discard it. */
	if (!lru->node->nr_items && list_empty(&lru->node->list))
		lru->node = NULL;
	spin_unlock(&lru->local_node.lock);
}

bool list_lru_add(struct list_lru *lru, struct list_head *item, int nid,
		  struct mem_cgroup *memcg)
{
	if (!lru || !lru->node || !item || nid != 0 || memcg) return false;
	struct list_lru_one *node = lru->node;
	bool added = false;
	spin_lock(&node->lock);
	if (list_empty(item) && node->nr_items < LONG_MAX) {
		list_add_tail(item, &node->list);
		node->nr_items++;
		lru->nr_items++;
		added = true;
	}
	spin_unlock(&node->lock);
	return added;
}

/* The caller already holds this node's lock, including during a walk. */
void list_lru_isolate(struct list_lru_one *node, struct list_head *item)
{
	list_del_init(item);
	node->nr_items--;
	node->owner->nr_items--;
}

void list_lru_isolate_move(struct list_lru_one *node, struct list_head *item,
			   struct list_head *head)
{
	list_move_tail(item, head);
	node->nr_items--;
	node->owner->nr_items--;
}

bool list_lru_del(struct list_lru *lru, struct list_head *item, int nid,
		  struct mem_cgroup *memcg)
{
	if (!lru || !lru->node || !item || nid != 0 || memcg) return false;
	struct list_lru_one *node = lru->node;
	bool removed = false;
	spin_lock(&node->lock);
	if (!list_empty(item)) {
		list_lru_isolate(node, item);
		removed = true;
	}
	spin_unlock(&node->lock);
	return removed;
}

unsigned long list_lru_count_node(struct list_lru *lru, int nid)
{
	if (!lru || !lru->node || nid != 0) return 0;
	struct list_lru_one *node = lru->node;
	spin_lock(&node->lock);
	unsigned long count = node->nr_items;
	spin_unlock(&node->lock);
	return count;
}

unsigned long list_lru_count(struct list_lru *lru)
{
	return list_lru_count_node(lru, 0);
}

unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
				 list_lru_walk_cb callback, void *arg,
				 unsigned long *nr_to_walk)
{
	if (!lru || !lru->node || nid != 0 || !callback || !nr_to_walk) return 0;
	struct list_lru_one *node = lru->node;
	unsigned long removed = 0;
	spin_lock(&node->lock);
restart:
	for (struct list_head *item = node->list.next, *next;
	     item != &node->list && *nr_to_walk; item = next) {
		next = item->next;
		--*nr_to_walk;
		switch (callback(item, node, arg)) {
		case LRU_REMOVED:
			removed++;
			break;
		case LRU_REMOVED_RETRY:
			removed++;
			goto restart;
		case LRU_ROTATE:
			list_move_tail(item, &node->list);
			break;
		case LRU_SKIP:
			break;
		case LRU_RETRY:
			goto restart;
		case LRU_STOP:
		default:
			goto done;
		}
	}
done:
	spin_unlock(&node->lock);
	return removed;
}

unsigned long list_lru_walk(struct list_lru *lru, list_lru_walk_cb callback,
			    void *arg, unsigned long nr_to_walk)
{
	return list_lru_walk_node(lru, 0, callback, arg, &nr_to_walk);
}
