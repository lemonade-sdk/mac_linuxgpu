/* linuxu: SHIM (third_party/linux/include/linux/eventfd.h) */
#ifndef __LINUX_EVENTFD_H
#define __LINUX_EVENTFD_H

#include <linux/err.h>

struct file;
struct eventfd_ctx;

static inline struct eventfd_ctx *eventfd_ctx_fdget(int fd)
{
	(void)fd;
	return ERR_PTR(-EINVAL);
}
static inline struct file *eventfd_fget(int fd) { (void)fd; return NULL; }
extern void eventfd_ctx_put(struct eventfd_ctx *ctx);
static inline void eventfd_signal(struct eventfd_ctx *ctx) { (void)ctx; }
#endif
