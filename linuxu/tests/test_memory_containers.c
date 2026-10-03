/* Offline regressions for the real sparse-ID, radix, interval and SG shims. */
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/idr.h>
#include <linux/ida.h>
#include <linux/radix-tree.h>
#include <linux/interval_tree.h>
#include <linux/scatterlist.h>
#include <linux/llist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/kernel.h>

extern size_t kmemcheck_live_bytes(void);
static void test_rcu_containers(void)
{
	struct item {
		unsigned long prefix[5];
		struct rcu_head rcu;
		struct list_head list;
		struct hlist_node hlist;
		int value;
	};
	struct list_head list = LIST_HEAD_INIT(list);
	struct hlist_head hash = HLIST_HEAD_INIT;
	struct item *position;
	unsigned int count = 0;
	rcu_barrier();
	size_t baseline = kmemcheck_live_bytes();
	list_for_each_entry_rcu(position, &list, list) { assert(0); }
	assert(!list_first_entry_or_null_rcu(&list, struct item, list));
	struct item *first = kzalloc(sizeof(*first), GFP_KERNEL);
	struct item *second = kzalloc(sizeof(*second), GFP_KERNEL);
	assert(first && second);
	first->value = 11; second->value = 17;
	list_add_rcu(&first->list, &list);
	list_add_tail_rcu(&second->list, &list);
	hlist_add_head_rcu(&second->hlist, &hash);
	hlist_add_head_rcu(&first->hlist, &hash);
	rcu_read_lock();
	assert(list_first_entry_or_null_rcu(&list, struct item, list) == first);
	list_for_each_entry_rcu(position, &list, list) {
		assert(position->value == (count++ ? 17 : 11));
	}
	assert(count == 2);
	struct hlist_node *saved = rcu_dereference(hash.first);
	hlist_del_init_rcu(&first->hlist);
	assert(hlist_unhashed(&first->hlist));
	assert(saved->next == &second->hlist); /* A concurrent reader can advance. */
	count = 0;
	hlist_for_each_entry_rcu(position, &hash, hlist) {
		assert(position == second); ++count;
	}
	assert(count == 1);
	list_del_rcu(&first->list);
	struct item *objects[] = {first, NULL};
	unsigned int evaluated = 0;
	kfree_rcu(objects[evaluated++], rcu);
	assert(evaluated == 1 && kmemcheck_live_bytes() == baseline + 2 * sizeof(*first));
	assert(first->value == 11 && first->list.next == &second->list);
	rcu_read_unlock();
	rcu_barrier();
	assert(kmemcheck_live_bytes() == baseline + sizeof(*second));
	list_del_rcu(&second->list);
	hlist_del_init_rcu(&second->hlist);
	kfree_rcu(second, rcu);
	kfree_rcu(objects[1], rcu);
	rcu_barrier();
	assert(list_empty(&list) && hlist_empty(&hash));
	assert(kmemcheck_live_bytes() == baseline);
}

static int values[128];
static int stop_walk(int id, void *entry, void *data)
{
	assert(id == 6 && entry == &values[2]);
	(*(int *)data)++;
	return 17;
}
static void test_idr(void)
{
	struct idr idr;
	idr_init_base(&idr, 5);
	assert(idr_alloc(&idr, &values[0], 0, 8, 0) == 5);
	assert(idr_alloc(&idr, NULL, 0, 8, 0) == 6);
	assert(idr_alloc(&idr, &values[1], 0, 8, 0) == 7);
	assert(idr_alloc(&idr, &values[0], 0, 8, 0) == -ENOSPC);
	assert(idr_replace(&idr, &values[2], 6) == NULL);
	assert(idr_replace(&idr, &values[3], 99) == ERR_PTR(-ENOENT));
	assert(idr_remove(&idr, 5) == &values[0]);
	int id, count = 0;
	void *entry;
	idr_for_each_entry(&idr, entry, id) {
		assert(id == 6 + count);
		assert(entry == &values[count == 0 ? 2 : 1]);
		count++;
	}
	assert(count == 2 && !entry);
	id = 6; count = 0;
	idr_for_each_entry_continue(&idr, entry, id) { count++; }
	assert(count == 2);
	count = 0;
	assert(idr_for_each(&idr, stop_walk, &count) == 17 && count == 1);
	idr_destroy(&idr);
	assert(idr_is_empty(&idr));
	idr_init(&idr);
	assert(idr_alloc(&idr, &values[0], INT_MAX, 0, 0) == INT_MAX);
	count = 0;
	idr_for_each_entry(&idr, entry, id) { assert(id == INT_MAX); count++; }
	assert(count == 1);
	idr_destroy(&idr);
	for (int i = 10; i <= 12; i++) assert(idr_alloc_cyclic(&idr, &values[0], 10, 13, 0) == i);
	assert(idr_alloc_cyclic(&idr, &values[0], 10, 13, 0) == -ENOSPC);
	idr_remove(&idr, 11);
	assert(idr_alloc_cyclic(&idr, &values[1], 10, 13, 0) == 11);
	idr_destroy(&idr);
}

#define THREADS 8
#define IDS_PER_THREAD 64
static struct ida ids;
static int allocated[THREADS][IDS_PER_THREAD];
static void *allocate_ids(void *argument)
{
	unsigned long thread = (unsigned long)argument;
	for (int i = 0; i < IDS_PER_THREAD; i++) {
		allocated[thread][i] = ida_alloc_range(&ids, 100, 611, 0);
		assert(allocated[thread][i] >= 100);
	}
	return NULL;
}
static void test_ida(void)
{
	pthread_t threads[THREADS];
	bool seen[THREADS * IDS_PER_THREAD] = {0};
	ida_init(&ids);
	for (unsigned long t = 0; t < THREADS; t++) assert(!pthread_create(&threads[t], NULL, allocate_ids, (void *)t));
	for (int t = 0; t < THREADS; t++) assert(!pthread_join(threads[t], NULL));
	for (int t = 0; t < THREADS; t++) for (int i = 0; i < IDS_PER_THREAD; i++) {
		int index = allocated[t][i] - 100;
		assert(!seen[index]); seen[index] = true;
	}
	assert(ida_alloc_range(&ids, 100, 611, 0) == -ENOSPC);
	ida_free(&ids, 333);
	assert(ida_alloc_range(&ids, 100, 611, 0) == 333);
	assert(ida_alloc_range(&ids, UINT_MAX, UINT_MAX, 0) == -ENOSPC);
	ida_destroy(&ids);
	assert(ida_alloc_range(&ids, INT_MAX, UINT_MAX, 0) == INT_MAX);
	ida_destroy(&ids);
}

static void test_radix(void)
{
	RADIX_TREE(tree, 0);
	unsigned long indices[] = {1, 64, 1000, 1UL << 40, ULONG_MAX};
	for (unsigned int i = 0; i < 5; i++) {
		int *entry = malloc(sizeof(*entry)); assert(entry); *entry = i;
		assert(!radix_tree_insert(&tree, indices[i], entry));
		assert(radix_tree_insert(&tree, indices[i], &values[0]) == -EEXIST);
		if (i & 1) radix_tree_tag_set(&tree, indices[i], 1);
	}
	void *found[5];
	assert(radix_tree_gang_lookup_tag(&tree, found, 0, 5, 1) == 2);
	assert(*(int *)found[0] == 1 && *(int *)found[1] == 3);
	assert(radix_tree_tagged(&tree, 1));
	radix_tree_tag_clear(&tree, indices[1], 1);
	assert(!radix_tree_tag_get(&tree, indices[1], 1));
	struct radix_tree_iter iter;
	void **slot;
	unsigned int count = 0;
	radix_tree_for_each_slot(slot, &tree, &iter, 1000) {
		assert(iter.index == indices[count + 2]); count++;
	}
	assert(count == 3);
	count = 0;
	/* This mirrors AMDGPU RAS cleanup: dereference, free, delete and continue. */
	radix_tree_for_each_slot(slot, &tree, &iter, 0) {
		assert(iter.index == indices[count]);
		int *entry = radix_tree_deref_slot(slot);
		assert(*entry == count++);
		free(entry);
		assert(!radix_tree_iter_delete(&tree, &iter, slot));
	}
	assert(count == 5 && radix_tree_empty(&tree));
}

static RADIX_TREE(rcu_tree, 0);
static pthread_mutex_t reader_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reader_changed = PTHREAD_COND_INITIALIZER;
static bool slot_acquired, writer_finished, destroy_tree;
static void *hold_radix_slot(void *unused)
{
	(void)unused;
	rcu_read_lock();
	void **slot = radix_tree_lookup_slot(&rcu_tree, 1UL << 40);
	assert(slot && radix_tree_deref_slot(slot) == &values[0]);
	pthread_mutex_lock(&reader_lock);
	slot_acquired = true;
	pthread_cond_broadcast(&reader_changed);
	while (!writer_finished) pthread_cond_wait(&reader_changed, &reader_lock);
	pthread_mutex_unlock(&reader_lock);
	/* Erase clears a retained slot; destroy detaches its subtree. Both must
	 * preserve the node storage until this reader leaves its RCU section. */
	assert(radix_tree_deref_slot(slot) == (destroy_tree ? &values[1] : NULL));
	rcu_read_unlock();
	return NULL;
}
static void test_radix_slot_lifetime(void)
{
	for (unsigned int mode = 0; mode < 2; mode++) {
		destroy_tree = mode;
		slot_acquired = writer_finished = false;
		assert(!radix_tree_insert(&rcu_tree, 1UL << 40, &values[0]));
		pthread_t thread;
		assert(!pthread_create(&thread, NULL, hold_radix_slot, NULL));
		pthread_mutex_lock(&reader_lock);
		while (!slot_acquired) pthread_cond_wait(&reader_changed, &reader_lock);
		pthread_mutex_unlock(&reader_lock);
		if (destroy_tree) {
			void **slot = radix_tree_lookup_slot(&rcu_tree, 1UL << 40);
			radix_tree_replace_slot(&rcu_tree, slot, &values[1]);
			xa_destroy(&rcu_tree);
		} else {
			assert(radix_tree_delete(&rcu_tree, 1UL << 40) == &values[0]);
		}
		pthread_mutex_lock(&reader_lock);
		writer_finished = true;
		pthread_cond_broadcast(&reader_changed);
		pthread_mutex_unlock(&reader_lock);
		assert(!pthread_join(thread, NULL));
		rcu_barrier();
		assert(radix_tree_empty(&rcu_tree));
	}
}

static void test_intervals(void)
{
	struct rb_root_cached tree = RB_ROOT_CACHED;
	struct interval_tree_node nodes[64] = {0};
	for (unsigned int i = 0; i < 64; i++) {
		nodes[i].start = (i * 37) % 251;
		nodes[i].last = nodes[i].start + i % 31;
		interval_tree_insert(&nodes[i], &tree);
	}
	for (unsigned int removed = 0; removed < 64; removed++) {
		for (unsigned long first = 0; first < 280; first += 7) {
			unsigned int expected = 0, found = 0;
			for (unsigned int i = removed; i < 64; i++)
				if (nodes[i].start <= first + 3 && first <= nodes[i].last) expected++;
			struct interval_tree_node *node = interval_tree_iter_first(&tree, first, first + 3);
			for (; node; node = interval_tree_iter_next(node, first, first + 3)) {
				assert(node >= &nodes[removed] && node < &nodes[64]);
				assert(node->start <= first + 3 && first <= node->last);
				assert(++found <= 64);
			}
			assert(found == expected);
		}
		interval_tree_remove(&nodes[removed], &tree);
	}
	assert(!tree.rb_root.rb_node && !tree.rb_leftmost);
}

static void test_scatterlist(void)
{
	struct page backing[3] = {0};
	struct page *pages[] = {&backing[0], &backing[1], &backing[2]};
	struct sg_table table;
	assert(!sg_alloc_table_from_pages_segment(&table, pages, 3, PAGE_SIZE - 4, PAGE_SIZE + 9, PAGE_SIZE, 0));
	assert(table.nents == 3 && table.orig_nents == 3);
	assert(table.sgl[0].offset == PAGE_SIZE - 4 && table.sgl[0].length == 4);
	assert(table.sgl[1].offset == 0 && table.sgl[1].length == PAGE_SIZE);
	assert(table.sgl[2].offset == 0 && table.sgl[2].length == 5);
	assert(sg_is_last(&table.sgl[2]) && !sg_next(&table.sgl[2]));
	assert(sg_nents_for_len(table.sgl, PAGE_SIZE + 9) == 3);
	assert(sg_nents_for_len(table.sgl, PAGE_SIZE + 10) == -EINVAL);
	assert(sg_nents_for_len(table.sgl, 0) == 0);
	struct sg_page_iter it;
	unsigned int count = 0;
	for_each_sgtable_page(&table, &it, 0) { assert(sg_page_iter_page(&it) == pages[count++]); }
	assert(count == 3);
	count = 1;
	for_each_sgtable_page(&table, &it, 1) { assert(sg_page_iter_page(&it) == pages[count++]); }
	assert(count == 3);
	for (unsigned int i = 0; i < 3; i++) sg_dma_address(&table.sgl[i]) = 0x20000 + i * PAGE_SIZE;
	struct sg_dma_page_iter dma_it;
	count = 0;
	for_each_sgtable_dma_page(&table, &dma_it, 0) {
		assert(sg_page_iter_dma_address(&dma_it) == 0x20000 + count++ * PAGE_SIZE);
	}
	assert(count == 3);
	sg_free_table(&table);
	assert(!table.sgl && !table.nents && !table.orig_nents);
	sg_free_table(&table);
	assert(sg_alloc_table_from_pages(&table, pages, 1, PAGE_SIZE, 1, 0) == -EINVAL);
	assert(sg_alloc_table_from_pages(&table, pages, 1, 0, PAGE_SIZE + 1, 0) == -EINVAL);
	assert(sg_alloc_table_from_pages_segment(&table, pages, 1, 0, 1, PAGE_SIZE - 1, 0) == -EINVAL);
	assert(!sg_alloc_table_from_pages(&table, pages, 3, 1, 9, 0));
	assert(table.nents == 1 && sg_is_last(table.sgl));
	sg_free_table(&table);
	struct scatterlist first[2], second[2];
	sg_init_table(first, 2); sg_init_table(second, 2);
	sg_set_page(&first[0], pages[0], 3, 0);
	sg_set_page(&second[0], pages[1], 5, 0);
	sg_set_page(&second[1], pages[2], 7, 0);
	sg_chain(first, 2, second);
	assert(sg_next(first) == second && sg_next(second) == &second[1]);
	assert(!sg_next(&second[1]) && sg_nents_for_len(first, 15) == 3);
}

static void test_llist(void)
{
	struct item { unsigned long prefix[9]; struct llist_node node; int value; };
	struct item *item, *next;
	struct llist_head head = LLIST_HEAD_INIT(head);
	unsigned int count = 0;
	/* DRM connector cleanup can run with an empty list and a nonzero member offset. */
	llist_for_each_entry_safe(item, next, llist_del_all(&head), node) { assert(0); }
	for (int i = 0; i < 3; i++) {
		item = calloc(1, sizeof(*item)); assert(item); item->value = i;
		assert(llist_add(&item->node, &head) == (i == 0));
	}
	llist_for_each_entry_safe(item, next, llist_del_all(&head), node) {
		assert(item->value == 2 - count++);
		free(item);
	}
	assert(count == 3 && llist_empty(&head));
	struct llist_node nodes[3] = {0};
	assert(llist_add(&nodes[2], &head));
	nodes[0].next = &nodes[1];
	assert(!llist_add_batch(&nodes[0], &nodes[1], &head));
	assert(llist_shift_node(&nodes[1], &head) == NULL);
	for (unsigned int i = 0; i < 3; i++) assert(llist_del_first(&head) == &nodes[i]);
	assert(!llist_del_first(&head));
}

int main(void)
{
	test_rcu_containers();
	test_llist(); test_idr(); test_ida(); test_radix(); test_radix_slot_lifetime(); test_intervals(); test_scatterlist();
	rcu_barrier();
	puts("memory containers: llist cleanup, sparse IDs, concurrent IDA, RAS-style radix cleanup, interval overlap and scatterlist geometry passed");
	return 0;
}
