/* linuxu shim: list_debug — list integrity hooks
 * (NOOP: CONFIG_DEBUG_LIST is off in the shim). */
#include <linux/list.h>

void list_debug_init(void)
{
}

void list_debug_list_add(struct list_head *new, struct list_head *head)
{
	(void)new;
	(void)head;
}

void list_debug_list_del(struct list_head *entry)
{
	(void)entry;
}

void list_debug_list_insert_before(struct list_head *new,
				   struct list_head *next)
{
	(void)new;
	(void)next;
}

void list_debug_list_insert_after(struct list_head *new,
				  struct list_head *prev)
{
	(void)new;
	(void)prev;
}

void list_debug_list_hardened(void)
{
}

/* list.h slow-path validators (CONFIG_DEBUG_LIST off: trust + report) */
bool __list_add_valid_or_report(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	(void)new; (void)prev; (void)next;
	return true;
}

bool __list_del_entry_valid_or_report(struct list_head *entry)
{
	(void)entry;
	return true;
}
