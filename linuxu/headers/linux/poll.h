/* linuxu: SHIM (third_party/linux/include/linux/poll.h) */
#ifndef __LINUX_POLL_H
#define __LINUX_POLL_H
#include <linux/types.h>
#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define EPOLLIN 0x001
#define EPOLLOUT 0x004

/* ---- poll helpers (upstream linux/poll.h subset) ---- */
#ifndef __poll_t
typedef unsigned int __poll_t;
#endif
struct poll_table_struct {
	void (*_qproc)(struct file *, wait_queue_head_t *, struct poll_table_struct *);
	__poll_t _key;
};
typedef struct poll_table_struct poll_table;
static inline void poll_wait(struct file *file, wait_queue_head_t *wq,
			      struct poll_table_struct *p)
{
	(void)file; (void)wq; (void)p;
}
#endif
