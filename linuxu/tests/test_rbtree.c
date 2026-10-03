#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <linux/rbtree_augmented.h>

#define ITEM_COUNT 256

struct item {
	int key;
	int subtree_max;
	struct rb_node rb;
};

static int verify_node(struct rb_node *node, struct rb_node *parent,
		       int lower, int upper, int augmented, int *maximum)
{
	if (!node) {
		*maximum = INT_MIN;
		return 1;
	}
	struct item *item = rb_entry(node, struct item, rb);
	assert(rb_parent(node) == parent);
	assert(item->key > lower && item->key < upper);
	if (rb_is_red(node)) {
		assert(!node->rb_left || rb_is_black(node->rb_left));
		assert(!node->rb_right || rb_is_black(node->rb_right));
	}
	int left_max, right_max;
	int left_black = verify_node(node->rb_left, node, lower, item->key,
				     augmented, &left_max);
	int right_black = verify_node(node->rb_right, node, item->key, upper,
				      augmented, &right_max);
	assert(left_black == right_black);
	*maximum = item->key;
	if (left_max > *maximum) *maximum = left_max;
	if (right_max > *maximum) *maximum = right_max;
	if (augmented) assert(item->subtree_max == *maximum);
	return left_black + rb_is_black(node);
}

static void verify_tree(struct rb_root *root, const unsigned char *present,
			int augmented)
{
	int maximum;
	if (root->rb_node) assert(rb_is_black(root->rb_node));
	verify_node(root->rb_node, NULL, INT_MIN, INT_MAX, augmented, &maximum);
	struct rb_node *cursor = rb_first(root);
	int previous = -1, count = 0;
	while (cursor) {
		struct item *item = rb_entry(cursor, struct item, rb);
		assert(item->key > previous && present[item->key]);
		previous = item->key;
		cursor = rb_next(cursor);
		count++;
	}
	int expected = 0;
	for (int i = 0; i < ITEM_COUNT; i++) expected += present[i];
	assert(count == expected);
	cursor = rb_last(root);
	previous = ITEM_COUNT;
	while (cursor) {
		int key = rb_entry(cursor, struct item, rb)->key;
		assert(key < previous);
		previous = key;
		cursor = rb_prev(cursor);
	}
}

static int item_value(struct item *item) { return item->key; }
RB_DECLARE_CALLBACKS_MAX(static, item_callbacks, struct item, rb,
			 int, subtree_max, item_value)

static void insert(struct rb_root *root, struct item *item, int augmented,
		   bool *leftmost)
{
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	*leftmost = true;
	while (*link) {
		parent = *link;
		if (item->key < rb_entry(parent, struct item, rb)->key)
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			*leftmost = false;
		}
	}
	rb_link_node(&item->rb, parent, link);
	if (augmented) {
		item_callbacks.propagate(parent, NULL);
		rb_insert_augmented(&item->rb, root, &item_callbacks);
	} else {
		rb_insert_color(&item->rb, root);
	}
}

int main(void)
{
	struct item items[ITEM_COUNT] = {0};
	struct item replacement = { .key = 42 };
	struct rb_node *nodes[ITEM_COUNT];
	unsigned char present[ITEM_COUNT] = {0};
	struct rb_root_cached cached = RB_ROOT_CACHED;
	for (int i = 0; i < ITEM_COUNT; i++) {
		int key = (i * 73) % ITEM_COUNT;
		items[key].key = key;
		nodes[key] = &items[key].rb;
		bool leftmost;
		insert(&cached.rb_root, &items[key], 0, &leftmost);
		if (leftmost) cached.rb_leftmost = &items[key].rb;
		present[key] = 1;
		verify_tree(&cached.rb_root, present, 0);
		assert(cached.rb_leftmost == rb_first(&cached.rb_root));
	}
	rb_replace_node(nodes[42], &replacement.rb, &cached.rb_root);
	nodes[42] = &replacement.rb;
	verify_tree(&cached.rb_root, present, 0);
	int postorder_count = 0;
	for (struct rb_node *n = rb_first_postorder(&cached.rb_root); n;
	     n = rb_next_postorder(n)) postorder_count++;
	assert(postorder_count == ITEM_COUNT);
	for (int i = 0; i < ITEM_COUNT; i++) {
		int key = (i * 91) % ITEM_COUNT;
		rb_erase_cached(nodes[key], &cached);
		present[key] = 0;
		verify_tree(&cached.rb_root, present, 0);
		assert(cached.rb_leftmost == rb_first(&cached.rb_root));
	}

	struct rb_root augmented = RB_ROOT;
	for (int i = 0; i < ITEM_COUNT; i++) {
		int key = (i * 37) % ITEM_COUNT;
		items[key].key = key;
		items[key].subtree_max = key;
		bool leftmost;
		insert(&augmented, &items[key], 1, &leftmost);
		present[key] = 1;
		verify_tree(&augmented, present, 1);
	}
	for (int i = 0; i < ITEM_COUNT; i++) {
		int key = (i * 53) % ITEM_COUNT;
		rb_erase_augmented(&items[key].rb, &augmented, &item_callbacks);
		present[key] = 0;
		verify_tree(&augmented, present, 1);
	}
	return 0;
}
