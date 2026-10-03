/* linuxu: SHIM (third_party/linux/include/linux/list_lru.h)
 *
 * Aligned to the 2026 pinned API used by third_party/linux/drivers/gpu/drm/ttm/ttm_pool.c:
 *   list_lru_add(lru, item, nid, memcg)   — item is struct list_head *
 *   list_lru_isolate(list_one, item)      — void
 *   list_lru_walk_node(lru, nid, cb, arg, nr_to_walk) — returns unsigned long
 * The single host NUMA node is embedded so TTM pool initialization cannot
 * silently lose pages after an allocation failure. Callbacks run under the
 * node lock and perform removals through list_lru_isolate[_move]().
 */
#ifndef _LINUX_LIST_LRU_H
#define _LINUX_LIST_LRU_H

#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct mem_cgroup;
struct list_lru;
struct list_lru_one;

enum lru_status {
	LRU_REMOVED,
	LRU_REMOVED_RETRY,
	LRU_ROTATE,
	LRU_SKIP,
	LRU_RETRY,
	LRU_STOP,
};

typedef enum lru_status (*list_lru_isolate_cb)(struct list_head *item,
					       struct list_lru_one *lru,
					       void *arg);
typedef enum lru_status (*list_lru_walk_cb)(struct list_head *item,
					    struct list_lru_one *lru,
					    void *arg);

struct list_lru_one {
	struct list_head	list;
	long		nr_items;
	spinlock_t	lock;
	struct list_lru	*owner;
};

struct list_lru {
	struct spinlock	lock;
	int		items_per_cpu;
	long		nr_items;
	struct list_lru_one	*node;
	struct list_lru_one	local_node;
};

struct list_lru_item {
	struct list_head	entry;
};

#define LIST_LRU_ITEM_INIT(name)	{ .entry = LIST_HEAD_INIT(name.entry) }
#define LIST_LRU_ITEM(name)		struct list_lru_item name = LIST_LRU_ITEM_INIT(name)

extern int list_lru_init(struct list_lru *lru);
extern void list_lru_destroy(struct list_lru *lru);
extern bool list_lru_add(struct list_lru *lru, struct list_head *item, int nid,
			 struct mem_cgroup *memcg);
extern bool list_lru_del(struct list_lru *lru, struct list_head *item, int nid,
			 struct mem_cgroup *memcg);
extern void list_lru_isolate(struct list_lru_one *list, struct list_head *item);
extern void list_lru_isolate_move(struct list_lru_one *list, struct list_head *item,
				  struct list_head *new_list);
extern unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
					 enum lru_status (*cb)(struct list_head *, struct list_lru_one *, void *),
					 void *arg,
					 unsigned long *nr_to_walk);
extern unsigned long list_lru_walk(struct list_lru *lru,
				   enum lru_status (*cb)(struct list_head *, struct list_lru_one *, void *),
				   void *arg,
				   unsigned long nr_to_walk);
extern unsigned long list_lru_count(struct list_lru *lru);
extern unsigned long list_lru_count_node(struct list_lru *lru, int nid);

#endif /* _LINUX_LIST_LRU_H */
