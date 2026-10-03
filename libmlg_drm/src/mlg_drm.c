/* libmlg_drm (mlg_drm.h): the system calls of a client of the Linux-file
 * RPC. Each ioctl is described (lx_describe.c), framed (lx_frame.c), sent
 * through the transport and its reply copied back into the caller's
 * memory. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mlg_drm.h"
#include "mlg_transport.h"
#include "mlg_uapi.h"
#include <rt/lx_abi.h>

/* What each descriptor is: indexed by descriptor, MLG_LX_DEV_* or 0. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct mlg_transport transport;
static bool have_transport;
static uint32_t *fd_dev;
static size_t fd_cap;
static unsigned int fds_open;

struct mapping {
	struct mapping *next;
	void *addr;
	uint64_t length;	/* as the caller asked */
	uint64_t span;		/* as mapped: whole pages */
	uint64_t handle;
};
static struct mapping *mappings;

static _Thread_local int last_linux_errno;

/* ---- errors ---- */

int mlg_errno_from_linux(int e)
{
	switch (e) {
	/* 1-34 are the same on every Unix. */
	case 1: return EPERM;
	case 2: return ENOENT;
	case 3: return ESRCH;
	case 4: return EINTR;
	case 5: return EIO;
	case 6: return ENXIO;
	case 7: return E2BIG;
	case 8: return ENOEXEC;
	case 9: return EBADF;
	case 10: return ECHILD;
	case 11: return EAGAIN;
	case 12: return ENOMEM;
	case 13: return EACCES;
	case 14: return EFAULT;
	case 16: return EBUSY;
	case 17: return EEXIST;
	case 18: return EXDEV;
	case 19: return ENODEV;
	case 20: return ENOTDIR;
	case 21: return EISDIR;
	case 22: return EINVAL;
	case 23: return ENFILE;
	case 24: return EMFILE;
	case 25: return ENOTTY;
	case 26: return ETXTBSY;
	case 27: return EFBIG;
	case 28: return ENOSPC;
	case 29: return ESPIPE;
	case 30: return EROFS;
	case 31: return EMLINK;
	case 32: return EPIPE;
	case 33: return EDOM;
	case 34: return ERANGE;
	/* Linux numbers from here on. */
	case 35: return EDEADLK;
	case 36: return ENAMETOOLONG;
	case 37: return ENOLCK;
	case 38: return ENOSYS;
	case 39: return ENOTEMPTY;
	case 40: return ELOOP;
	case 42: return ENOMSG;
	case 43: return EIDRM;
	case 61: return ENODATA;
	case 62: return ETIME;
	case 71: return EPROTO;
	case 74: return EBADMSG;
	case 75: return EOVERFLOW;
	case 84: return EILSEQ;
	case 95: return EOPNOTSUPP;
	case 105: return ENOBUFS;
	case 110: return ETIMEDOUT;
	case 125: return ECANCELED;
	case 130: return EOWNERDEAD;
	case 131: return ENOTRECOVERABLE;
	default: return EIO;
	}
}

int mlg_last_linux_errno(void)
{
	return last_linux_errno;
}

static int fail(int linux_errno)
{
	last_linux_errno = linux_errno;
	errno = mlg_errno_from_linux(linux_errno);
	return -1;
}

/* ---- transport and descriptors ---- */

static bool get_transport(struct mlg_transport *out)
{
	bool have;

	pthread_mutex_lock(&lock);
	if (!have_transport && !mlg_default_transport(&transport))
		have_transport = true;
	*out = transport;
	have = have_transport;
	pthread_mutex_unlock(&lock);
	return have;
}

int mlg_drm_set_transport(const struct mlg_transport *t)
{
	int r = 0;

	pthread_mutex_lock(&lock);
	if (fds_open || mappings) {
		r = -1;
	} else if (t) {
		transport = *t;
		have_transport = true;
	} else {
		have_transport = false;
		memset(&transport, 0, sizeof(transport));
	}
	pthread_mutex_unlock(&lock);
	if (r)
		errno = EBUSY;
	return r;
}

static uint32_t device_of(int fd)
{
	uint32_t dev = 0;

	pthread_mutex_lock(&lock);
	if (fd >= 0 && (size_t)fd < fd_cap)
		dev = fd_dev[fd];
	pthread_mutex_unlock(&lock);
	return dev;
}

static bool remember(int fd, uint32_t dev)
{
	bool ok = true;

	pthread_mutex_lock(&lock);
	if ((size_t)fd >= fd_cap) {
		size_t cap = fd_cap ? fd_cap : 64;
		uint32_t *grown;

		while (cap <= (size_t)fd)
			cap *= 2;
		grown = realloc(fd_dev, cap * sizeof(*grown));
		if (grown) {
			memset(grown + fd_cap, 0, (cap - fd_cap) * sizeof(*grown));
			fd_dev = grown;
			fd_cap = cap;
		} else {
			ok = false;
		}
	}
	if (ok) {
		if (!fd_dev[fd])
			fds_open++;
		fd_dev[fd] = dev;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static void forget(int fd)
{
	pthread_mutex_lock(&lock);
	if (fd >= 0 && (size_t)fd < fd_cap && fd_dev[fd]) {
		fd_dev[fd] = 0;
		fds_open--;
	}
	pthread_mutex_unlock(&lock);
}

/* ---- open/close ---- */

static int device_of_path(const char *path, uint32_t *dev)
{
	static const char render[] = "/dev/dri/renderD";

	if (!path)
		return -MLG_LX_EFAULT;
	if (!strcmp(path, "/dev/kfd")) {
		*dev = MLG_LX_DEV_KFD;
		return 0;
	}
	if (!strncmp(path, render, sizeof(render) - 1)) {
		const char *p = path + sizeof(render) - 1;
		char *end = NULL;
		long minor = strtol(p, &end, 10);

		/* The GPU's render node; one GPU per driver for now. */
		if (end != p && !*end && minor >= 128 && minor < 192) {
			*dev = MLG_LX_DEV_RENDER;
			return 0;
		}
	}
	return -MLG_LX_ENOENT;
}

int mlg_open(const char *path, int flags)
{
	struct mlg_transport t;
	uint32_t dev = 0, lflags;
	int r;

	r = device_of_path(path, &dev);
	if (r)
		return fail(-r);
	if (!get_transport(&t))
		return fail(MLG_LX_ENODEV);
	lflags = (flags & O_ACCMODE) == O_RDWR ? MLG_LX_O_RDWR :
		 (flags & O_ACCMODE) == O_WRONLY ? 1u : 0u;
	if (flags & O_CLOEXEC)
		lflags |= MLG_LX_O_CLOEXEC;
	if (flags & O_NONBLOCK)
		lflags |= MLG_LX_O_NONBLOCK;
	r = t.open(t.ctx, dev, lflags);
	if (r < 0)
		return fail(-r);
	if (!remember(r, dev)) {
		(void)t.close(t.ctx, r);
		return fail(MLG_LX_ENOMEM);
	}
	return r;
}

int mlg_close(int fd)
{
	struct mlg_transport t;
	int r;

	if (!get_transport(&t))
		return fail(MLG_LX_EBADF);
	r = t.close(t.ctx, fd);
	if (r < 0)
		return fail(-r);
	forget(fd);
	return 0;
}

/* ---- ioctl ---- */

unsigned long mlg_ioctl_from_bsd(unsigned long request)
{
	const uint32_t r = (uint32_t)request;
	uint32_t dir = 0;

	if (r & 0x80000000u)	/* IOC_IN: the kernel reads */
		dir |= 1u;	/* _IOC_WRITE */
	if (r & 0x40000000u)	/* IOC_OUT: the kernel writes */
		dir |= 2u;	/* _IOC_READ */
	return (unsigned long)((dir << 30) | (((r >> 16) & 0x1fffu) << 16) | (r & 0xffffu));
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The DRM core finds a driver request by its number and copies only the
 * directions both the caller's encoding and its own table declare, so a
 * caller may encode a request with more directions than the table (Mesa
 * issues DRM_AMDGPU_GEM_VA as read-write; the table declares write). The
 * transport knows requests by their exact encoding: send the table's. */
static uint32_t table_cmd(uint32_t dev, uint32_t cmd)
{
	/* The render node's requests are known by exact encoding, so the
	 * one that matches is the table's; KFD's are known by range. */
	if (dev != MLG_LX_DEV_RENDER || mlg_lx_cmd_known(dev, cmd))
		return cmd;
	for (uint32_t dir = 0; dir < 4; ++dir) {
		const uint32_t c = (cmd & ~(3u << 30)) | dir << 30;

		if (mlg_lx_cmd_known(dev, c))
			return c;
	}
	return cmd;
}

int mlg_ioctl(int fd, unsigned long request, void *arg)
{
	struct mlg_lx_span local[32], *spans = local;
	uint8_t frame_local[2048], reply_local[2048];
	void *frame = frame_local, *reply = reply_local;
	const uint32_t dev = device_of(fd);
	const uint32_t cmd = table_cmd(dev, (uint32_t)request);
	uint64_t timeout_va = 0, out_bytes = 0;
	struct mlg_transport t;
	size_t reply_bytes = 0;
	int64_t result = 0;
	uint32_t n = 0;
	bool async;
	long len;
	int r;

	if (!dev)
		return fail(MLG_LX_EBADF);
	if (!get_transport(&t))
		return fail(MLG_LX_EBADF);
	r = mlg_lx_describe(dev, cmd, (uint64_t)(uintptr_t)arg, spans, 32, &n, &timeout_va);
	if (r == -MLG_LX_E2BIG) {
		spans = calloc(MLG_LX_DESCRIBE_MAX, sizeof(*spans));
		if (!spans)
			return fail(MLG_LX_ENOMEM);
		r = mlg_lx_describe(dev, cmd, (uint64_t)(uintptr_t)arg, spans, MLG_LX_DESCRIBE_MAX,
				    &n, &timeout_va);
	}
	if (r)
		goto out;
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va, monotonic_ns(),
			    NULL, 0, &out_bytes);
	if (len < 0) {
		r = (int)len;
		goto out;
	}
	if ((size_t)len > sizeof(frame_local) && !(frame = malloc((size_t)len))) {
		r = -MLG_LX_ENOMEM;
		goto out;
	}
	if (mlg_lx_reply_bytes(out_bytes) > sizeof(reply_local) &&
	    !(reply = malloc(mlg_lx_reply_bytes(out_bytes)))) {
		r = -MLG_LX_ENOMEM;
		goto out;
	}
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va, monotonic_ns(),
			    frame, (size_t)len, &out_bytes);
	if (len < 0) {
		r = (int)len;
		goto out;
	}
	/* A wait goes to a worker of the driver's process, unless it only
	 * polls (a zero deadline). */
	async = mlg_lx_cmd_blocks(dev, cmd);
	if (async && timeout_va) {
		int64_t deadline;

		memcpy(&deadline, (const void *)(uintptr_t)timeout_va, sizeof(deadline));
		async = deadline != 0;
	}
	r = t.ioctl(t.ctx, fd, cmd, frame, (size_t)len, reply, mlg_lx_reply_bytes(out_bytes),
		    &reply_bytes, &result, async);
	if (!r && mlg_lx_apply_reply(frame, (size_t)len, reply, reply_bytes, NULL))
		r = -MLG_LX_EINVAL;
out:
	if (spans != local)
		free(spans);
	if (frame != frame_local)
		free(frame);
	if (reply != reply_local)
		free(reply);
	if (r)
		return fail(-r);
	if (result < 0)
		return fail((int)-result);
	last_linux_errno = 0;
	return (int)result;
}

/* ---- mmap ---- */

void *mlg_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	struct mlg_transport t;
	struct mapping *m;
	uint32_t lprot = 0;
	void *at = NULL;
	uint64_t handle = 0;
	int r;

	(void)addr;	/* a hint only; the driver places the mapping */
	if (!device_of(fd)) {
		fail(MLG_LX_EBADF);
		return MAP_FAILED;
	}
	if (!length || offset < 0 || (flags & MAP_FIXED) || !(flags & MAP_SHARED) ||
	    (prot & ~(PROT_READ | PROT_WRITE))) {
		fail(MLG_LX_EINVAL);
		return MAP_FAILED;
	}
	if (prot & PROT_READ)
		lprot |= MLG_LX_PROT_READ;
	if (prot & PROT_WRITE)
		lprot |= MLG_LX_PROT_WRITE;
	if (!get_transport(&t)) {
		fail(MLG_LX_ENODEV);
		return MAP_FAILED;
	}
	m = calloc(1, sizeof(*m));
	if (!m) {
		fail(MLG_LX_ENOMEM);
		return MAP_FAILED;
	}
	/* As mmap(2) does, map whole pages (the driver process's page size
	 * is this platform's); munmap takes the caller's length. */
	const uint64_t page = (uint64_t)getpagesize();
	const uint64_t span = ((uint64_t)length + page - 1) & ~(page - 1);

	r = t.mmap(t.ctx, fd, (uint64_t)offset, span, lprot, MLG_LX_MAP_SHARED, &at, &handle);
	if (r) {
		free(m);
		fail(-r);
		return MAP_FAILED;
	}
	m->addr = at;
	m->length = length;
	m->span = span;
	m->handle = handle;
	pthread_mutex_lock(&lock);
	m->next = mappings;
	mappings = m;
	pthread_mutex_unlock(&lock);
	return at;
}

int mlg_is_mapping(const void *addr, size_t length)
{
	int found = 0;

	pthread_mutex_lock(&lock);
	for (struct mapping *m = mappings; m && !found; m = m->next)
		found = m->addr == addr && m->length == length;
	pthread_mutex_unlock(&lock);
	return found;
}

int mlg_munmap(void *addr, size_t length)
{
	struct mlg_transport t;
	struct mapping *m = NULL;
	int r;

	pthread_mutex_lock(&lock);
	for (struct mapping **link = &mappings; *link; link = &(*link)->next) {
		if ((*link)->addr == addr && (*link)->length == length) {
			m = *link;
			*link = m->next;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	/* Whole mappings only: the driver maps a buffer as one object. */
	if (!m)
		return fail(MLG_LX_EINVAL);
	if (!get_transport(&t)) {
		free(m);
		return fail(MLG_LX_ENODEV);
	}
	r = t.munmap(t.ctx, m->handle, m->addr, m->span);
	free(m);
	return r ? fail(-r) : 0;
}

/* ---- the GPU's PCI identity ---- */

int mlg_pci_identity(struct mlg_pci_identity *out)
{
	struct drm_amdgpu_info_device info = { 0 };
	struct drm_amdgpu_info request = {
		.return_pointer = (uint64_t)(uintptr_t)&info,
		.return_size = sizeof(info),
		.query = AMDGPU_INFO_DEV_INFO,
	};
	struct mlg_transport t;
	int fd, r;

	if (!out)
		return fail(MLG_LX_EFAULT);
	if (!get_transport(&t))
		return fail(MLG_LX_ENODEV);
	memset(out, 0, sizeof(*out));
	if (t.identity) {
		r = t.identity(t.ctx, out);
		if (r)
			return fail(-r);
		last_linux_errno = 0;
		return 0;
	}
	/* What the driver itself reports; AMD is the only vendor amdgpu
	 * drives. */
	fd = mlg_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	r = mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &request);
	if (r) {
		int e = last_linux_errno;

		(void)mlg_close(fd);
		return fail(e);
	}
	(void)mlg_close(fd);
	out->vendor_id = 0x1002;
	out->device_id = (uint16_t)info.device_id;
	out->revision_id = (uint8_t)info.pci_rev;
	last_linux_errno = 0;
	return 0;
}
