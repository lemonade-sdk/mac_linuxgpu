/* linuxu: SHIM (third_party/linux/include/linux/list_sort.h) */
#ifndef __LINUX_LIST_SORT_H
#define __LINUX_LIST_SORT_H

#include <linux/list.h>

typedef int (*list_cmp_func_t)(void *priv, const struct list_head *a,
			       const struct list_head *b);

void list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp);

#endif
