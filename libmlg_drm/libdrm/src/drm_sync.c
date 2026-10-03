/* libdrm for mac_linuxgpu: sync_file helpers (libsync.h) through the
 * driver's syncobjs.
 *
 * The driver's sync_files take no requests of their own, so each helper
 * works on temporary syncobjs of a render-node file: one the process has
 * open, or else one opened for the call. A wait imports the sync_file into
 * a syncobj and waits on it; a merge transfers both fences to consecutive
 * points of a timeline syncobj and exports the later point, whose fence
 * chain signals once both have. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "libsync.h"
#include "mlg_drm.h"
#include "drm_internal.h"

/* One request on driver file @dfd: 0 or -errno. */
static int call(int dfd, unsigned long req, void *arg)
{
	int r;

	do {
		r = mlg_ioctl(dfd, mlg_ioctl_from_bsd(req), arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r ? -errno : 0;
}

static int syncobj_create(int dev, uint32_t *handle)
{
	struct drm_syncobj_create c = { 0 };
	int r = call(dev, DRM_IOCTL_SYNCOBJ_CREATE, &c);

	*handle = c.handle;
	return r;
}

static void syncobj_destroy(int dev, uint32_t handle)
{
	struct drm_syncobj_destroy d = { .handle = handle };

	if (handle)
		(void)call(dev, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

/* A syncobj holding the fence of sync_file @fd. */
static int import(int dev, int fd, uint32_t *handle)
{
	struct drm_syncobj_handle h = { 0 };
	int dfd = drm_file_driver_fd(fd), r;

	*handle = 0;
	if (dfd < 0)
		return -EINVAL;
	r = syncobj_create(dev, handle);
	if (r)
		return r;
	h.handle = *handle;
	h.fd = dfd;
	h.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	r = call(dev, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h);
	if (r) {
		syncobj_destroy(dev, *handle);
		*handle = 0;
	}
	return r;
}

static int wait_on(int dev, int fd, int timeout)
{
	struct drm_syncobj_wait w = { 0 };
	struct timespec now;
	uint32_t handle;
	int r = import(dev, fd, &handle);

	if (r)
		return r;
	clock_gettime(CLOCK_MONOTONIC, &now);
	w.handles = (uintptr_t)&handle;
	w.count_handles = 1;
	w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	w.timeout_nsec = timeout < 0 ? INT64_MAX :
		(int64_t)now.tv_sec * 1000000000ll + now.tv_nsec + (int64_t)timeout * 1000000ll;
	r = call(dev, DRM_IOCTL_SYNCOBJ_WAIT, &w);
	syncobj_destroy(dev, handle);
	return r;
}

/* The merge on device file @dev: 0 with the driver's sync_file in *@out,
 * or -errno. */
static int merge_on(int dev, int fd1, int fd2, int *out_fd)
{
	struct drm_syncobj_transfer t = { 0 };
	struct drm_syncobj_handle out = { 0 };
	uint32_t a = 0, b = 0, chain = 0;
	int r;

	r = import(dev, fd1, &a);
	if (!r)
		r = import(dev, fd2, &b);
	if (!r)
		r = syncobj_create(dev, &chain);
	if (!r) {
		t = (struct drm_syncobj_transfer){ .src_handle = a, .dst_handle = chain,
						   .dst_point = 1 };
		r = call(dev, DRM_IOCTL_SYNCOBJ_TRANSFER, &t);
	}
	if (!r) {
		t = (struct drm_syncobj_transfer){ .src_handle = b, .dst_handle = chain,
						   .dst_point = 2 };
		r = call(dev, DRM_IOCTL_SYNCOBJ_TRANSFER, &t);
	}
	if (!r) {
		out.handle = chain;
		out.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
			    DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
		out.point = 2;
		out.fd = -1;
		r = call(dev, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &out);
	}
	syncobj_destroy(dev, a);
	syncobj_destroy(dev, b);
	syncobj_destroy(dev, chain);
	*out_fd = out.fd;
	return r;
}

/* Run @op on an open device file, or on one opened for the call when none
 * is open or the one found was closed meanwhile. */
struct sync_op {
	int kind;	/* 0: wait, 1: merge */
	int fd1, fd2, timeout;
	int out_fd;
};

static int run(int dev, struct sync_op *op)
{
	return op->kind ? merge_on(dev, op->fd1, op->fd2, &op->out_fd) :
			  wait_on(dev, op->fd1, op->timeout);
}

static int with_device(struct sync_op *op)
{
	int dev = drm_file_any_node(), r = -EBADF;

	if (dev >= 0)
		r = run(dev, op);
	if (r == -EBADF) {
		dev = mlg_open(DRM_MLG_RENDER_PATH, O_RDWR | O_CLOEXEC);
		if (dev < 0)
			return -errno;
		r = run(dev, op);
		(void)mlg_close(dev);
	}
	return r;
}

int sync_wait(int fd, int timeout)
{
	struct sync_op op = { .kind = 0, .fd1 = fd, .timeout = timeout };
	int r;

	if (drm_file_driver_fd(fd) < 0) {
		errno = EINVAL;
		return -1;
	}
	r = with_device(&op);
	if (r) {
		errno = r == -ETIMEDOUT ? ETIME : -r;
		return -1;
	}
	return 0;
}

int sync_merge(const char *name, int fd1, int fd2)
{
	struct sync_op op = { .kind = 1, .fd1 = fd1, .fd2 = fd2, .out_fd = -1 };
	int r;

	(void)name;
	if (drm_file_driver_fd(fd1) < 0 || drm_file_driver_fd(fd2) < 0) {
		errno = EINVAL;
		return -1;
	}
	r = with_device(&op);
	if (r) {
		errno = -r;
		return -1;
	}
	return drm_file_wrap(op.out_fd, true);
}

int sync_accumulate(const char *name, int *fd1, int fd2)
{
	int r;

	if (*fd1 < 0) {
		*fd1 = dup(fd2);
		return 0;
	}
	r = sync_merge(name, *fd1, fd2);
	if (r < 0)
		return r;
	close(*fd1);
	*fd1 = r;
	return 0;
}
