/* linuxu: SHIM (third_party/linux/include/linux/swap.h)
 *
 * Page-swap subsystem is not used by the host build; only the
 * memory-pressure notifier hooks the driver references.
 */
#ifndef _LINUX_SWAP_H
#define _LINUX_SWAP_H

#include <linux/notifier.h>
#include <linux/gfp.h>
#include <linux/shmem_fs.h>

enum swap_event {
	SWAP_SUSPEND,
	SWAP_RESUME,
};

struct swap_event_data { int dummy; };

/* the shim has no kswapd; ttm_bo_util.c consults it on allocation retries */
static inline bool current_is_kswapd(void) { return false; }

/* the shim has no swap pages; ttm_backup_bytes_avail() uses this */
static inline long get_nr_swap_pages(void) { return 0; }

#endif /* _LINUX_SWAP_H */
