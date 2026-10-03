// SPDX-License-Identifier: GPL-2.0-or-later
/* Instantiate the pinned Linux augmented-tree implementation. */
#include <linux/interval_tree.h>
#include <linux/interval_tree_generic.h>

#define interval_start(node) ((node)->start)
#define interval_last(node) ((node)->last)
INTERVAL_TREE_DEFINE(struct interval_tree_node, rb, unsigned long,
		    __subtree_last, interval_start, interval_last, , interval_tree)
