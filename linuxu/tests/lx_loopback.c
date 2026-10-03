/* A libmlg_drm transport that calls the Linux-file core (rt/lx_files.h)
 * in-process, as the dext's selectors do: one client, waits on the core's
 * async workers, mmaps of CPU-backed memory handed back as the dext
 * mapping itself (host memory in the host build). For tests that run a
 * libmlg_drm client against the fixture device. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <rt/lx_files.h>

#include "mlg_drm.h"
#include "lx_loopback.h"

struct waiter {
	struct completion done;
	int64_t result;
	void *reply;
	size_t cap, bytes;
	int kept;
};

static struct rt_lx_client *client;
static unsigned int async_calls;

static int lb_open(void *ctx, uint32_t dev, uint32_t flags)
{
	(void)ctx;
	return rt_lx_open(client, dev, flags);
}

static int lb_close(void *ctx, int fd)
{
	(void)ctx;
	return rt_lx_close(client, fd);
}

static int lb_done(void *ctx, uint64_t token, int64_t result, const void *rbuf, size_t bytes)
{
	struct waiter *w = ctx;

	(void)token;
	w->result = result;
	w->bytes = bytes;
	if (bytes <= w->cap)
		memcpy(w->reply, rbuf, bytes);
	else
		w->kept = 1;
	complete(&w->done);
	return !w->kept;
}

static int lb_ioctl(void *ctx, int fd, uint32_t cmd, const void *frame, size_t bytes, void *reply,
		    size_t cap, size_t *reply_bytes, int64_t *result, int async)
{
	struct waiter w = { .reply = reply, .cap = cap };
	uint64_t token = 0;
	int r;

	(void)ctx;
	if (!async)
		return rt_lx_ioctl(client, fd, cmd, frame, bytes, reply, cap, reply_bytes, result);
	__atomic_add_fetch(&async_calls, 1, __ATOMIC_SEQ_CST);
	init_completion(&w.done);
	r = rt_lx_ioctl_async(client, fd, cmd, frame, bytes, lb_done, &w, &token);
	if (r)
		return r;
	wait_for_completion(&w.done);
	if (w.kept)
		return rt_lx_result(client, token, reply, cap, reply_bytes, result);
	*reply_bytes = w.bytes;
	*result = w.result;
	return 0;
}

static int lb_mmap(void *ctx, int fd, uint64_t offset, uint64_t length, uint32_t prot,
		   uint32_t flags, void **addr, uint64_t *handle)
{
	struct rt_lx_map_info info;
	uint64_t contiguous = 0;
	void *cpu;
	int r;

	(void)ctx;
	r = rt_lx_mmap(client, fd, offset, length, prot, flags, &info);
	if (r)
		return r;
	cpu = rt_lx_map_cpu(client, info.type, 0, &contiguous);
	if (!cpu || contiguous < length) {
		/* A BAR range or scattered pages: the dext maps those as a
		 * descriptor; the loopback cannot. */
		rt_lx_munmap(client, info.type);
		return -ENODEV;
	}
	(void)rt_lx_mmap_commit(client, info.type, (uint64_t)(uintptr_t)cpu);
	*addr = cpu;
	*handle = info.type;
	return 0;
}

static int lb_munmap(void *ctx, uint64_t handle, void *addr, uint64_t length)
{
	(void)ctx;
	(void)addr;
	(void)length;
	return rt_lx_munmap(client, handle);
}

int lx_loopback_transport(struct pci_dev *pdev, struct mlg_transport *out)
{
	int r = rt_lx_client_create(pdev, 0, "mlg-loopback", &client);

	if (r)
		return r;
	*out = (struct mlg_transport){
		.open = lb_open, .close = lb_close, .ioctl = lb_ioctl,
		.mmap = lb_mmap, .munmap = lb_munmap,
	};
	return 0;
}

void lx_loopback_exit(void)
{
	rt_lx_client_destroy(client);
	client = NULL;
}

unsigned int lx_loopback_async_calls(void)
{
	return __atomic_load_n(&async_calls, __ATOMIC_SEQ_CST);
}

unsigned int lx_loopback_open_files(void)
{
	return rt_lx_open_files(client);
}
