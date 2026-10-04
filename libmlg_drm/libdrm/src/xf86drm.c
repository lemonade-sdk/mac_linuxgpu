/* libdrm for mac_linuxgpu: the core API (xf86drm.h) over libmlg_drm.
 *
 * Requests reach the driver's DRM files with mlg_ioctl, converted from the
 * BSD request encoding this platform's <drm.h> produces. Requests that
 * carry file descriptors have them translated between this process's DRM
 * descriptors (drm_file.c) and the driver's. Return conventions follow
 * libdrm (MIT; Copyright 1999 Precision Insight, Inc., 2000 VA Linux
 * Systems, Inc.) function by function. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "amdgpu_drm.h"
#include "mlg_drm.h"
#include "drm_internal.h"

#define memclear(s) memset(&(s), 0, sizeof(s))

/* ---- requests ---- */

/* The O_* flags of a PRIME export in the driver's (Linux) values. */
static uint32_t prime_flags_to_linux(uint32_t flags)
{
	uint32_t out = flags & ~(uint32_t)(O_CLOEXEC | O_ACCMODE);

	if (flags & O_CLOEXEC)
		out |= DRM_MLG_LINUX_O_CLOEXEC;
	if ((flags & O_ACCMODE) == O_RDWR)
		out |= DRM_MLG_LINUX_O_RDWR;
	return out;
}

/* One request on driver file @dfd, with the descriptors it carries
 * translated. Returns as ioctl(2) does. */
static int request(int dfd, unsigned long req, void *arg)
{
	const unsigned long cmd = mlg_ioctl_from_bsd(req);
	int r, in_fd, *fd_field = NULL, *out_fd = NULL;
	uint32_t saved_flags = 0, *flags_field = NULL;
	bool cloexec = true;

	/* Descriptors the request reads: the caller's proxies, as the
	 * driver's descriptors. (Requests here are in this platform's
	 * encoding, as the constants of <drm.h> are.) */
	switch ((uint32_t)req) {
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
		fd_field = &((struct drm_prime_handle *)arg)->fd;
		break;
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
		fd_field = &((struct drm_syncobj_handle *)arg)->fd;
		break;
	case DRM_IOCTL_SYNCOBJ_EVENTFD:
		/* No eventfd on this platform, nor in the driver
		 * (CONFIG_EVENTFD=n). */
		errno = EOPNOTSUPP;
		return -1;
	case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
		struct drm_prime_handle *p = arg;

		cloexec = !!(p->flags & O_CLOEXEC);
		flags_field = &p->flags;
		out_fd = &p->fd;
		break;
	}
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
		out_fd = &((struct drm_syncobj_handle *)arg)->fd;
		break;
	default:
		break;
	}
	if (fd_field) {
		in_fd = *fd_field;
		*fd_field = drm_file_driver_fd(in_fd);
		if (*fd_field < 0) {
			*fd_field = in_fd;
			errno = EBADF;
			return -1;
		}
	}
	if (flags_field) {
		saved_flags = *flags_field;
		*flags_field = prime_flags_to_linux(saved_flags);
	}

	r = mlg_ioctl(dfd, cmd, arg);

	if (fd_field)
		*fd_field = in_fd;
	if (flags_field)
		*flags_field = saved_flags;
	if (r)
		return r;

	/* Descriptors the request returns: driver descriptors, wrapped. */
	if (out_fd) {
		int proxy = drm_file_wrap(*out_fd, cloexec);

		if (proxy < 0)
			return -1;
		*out_fd = proxy;
	}
	return r;
}

/* LIBDRM_MLG_DEBUG=1: report every request and mapping that fails. */
static bool debug_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0)
		enabled = getenv("LIBDRM_MLG_DEBUG") && *getenv("LIBDRM_MLG_DEBUG") != '0';
	return enabled;
}

static int report(const char *what, unsigned long req, int r)
{
	if (r && debug_enabled()) {
		const int e = errno;

		fprintf(stderr, "libdrm-mlg: %s 0x%08lx (nr 0x%02lx) failed: %s (Linux errno %d)\n",
			what, req, req & 0xff, strerror(e), mlg_last_linux_errno());
		errno = e;
	}
	return r;
}

static int drm_ioctl_fd(int fd, unsigned long req, void *arg);

/* LIBDRM_MLG_STATS=1: count every request by number with the time it took
 * (the round trip to the driver, waits included) and print the table at
 * exit. Per-number slots, updated atomically: no lock on the hot path. */
struct request_stat {
	uint64_t count, ns, max_ns, failures;
};
static struct request_stat request_stats[256];
static struct request_stat mmap_stat, munmap_stat;
/* Syncobj waits with a zero timeout (polls: a failure is "not yet"),
 * apart from the ones that may sleep. */
static struct request_stat wait_poll_stat, timeline_wait_poll_stat;
static int stats_enabled = -1;

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static const char *request_name(unsigned int nr)
{
	switch (nr) {
	case 0x00: return "VERSION";
	case 0x09: return "GEM_CLOSE";
	case 0x0c: return "GET_CAP";
	case 0x2d: return "PRIME_HANDLE_TO_FD";
	case 0x2e: return "PRIME_FD_TO_HANDLE";
	case 0xbf: return "SYNCOBJ_CREATE";
	case 0xc0: return "SYNCOBJ_DESTROY";
	case 0xc1: return "SYNCOBJ_HANDLE_TO_FD";
	case 0xc2: return "SYNCOBJ_FD_TO_HANDLE";
	case 0xc3: return "SYNCOBJ_WAIT";
	case 0xc4: return "SYNCOBJ_RESET";
	case 0xc5: return "SYNCOBJ_SIGNAL";
	case 0xca: return "SYNCOBJ_TIMELINE_WAIT";
	case 0xcb: return "SYNCOBJ_QUERY";
	case 0xcc: return "SYNCOBJ_TRANSFER";
	case 0xcd: return "SYNCOBJ_TIMELINE_SIGNAL";
	case 0x40: return "AMDGPU_GEM_CREATE";
	case 0x41: return "AMDGPU_GEM_MMAP";
	case 0x42: return "AMDGPU_CTX";
	case 0x43: return "AMDGPU_BO_LIST";
	case 0x44: return "AMDGPU_CS";
	case 0x45: return "AMDGPU_INFO";
	case 0x46: return "AMDGPU_GEM_METADATA";
	case 0x47: return "AMDGPU_GEM_WAIT_IDLE";
	case 0x48: return "AMDGPU_GEM_VA";
	case 0x49: return "AMDGPU_WAIT_CS";
	case 0x50: return "AMDGPU_GEM_OP";
	case 0x51: return "AMDGPU_GEM_USERPTR";
	case 0x52: return "AMDGPU_WAIT_FENCES";
	case 0x53: return "AMDGPU_VM";
	case 0x54: return "AMDGPU_FENCE_TO_HANDLE";
	case 0x55: return "AMDGPU_SCHED";
	default: return NULL;
	}
}

static void stat_add(struct request_stat *st, uint64_t ns, bool failed)
{
	uint64_t max = __atomic_load_n(&st->max_ns, __ATOMIC_RELAXED);

	__atomic_add_fetch(&st->count, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&st->ns, ns, __ATOMIC_RELAXED);
	if (failed)
		__atomic_add_fetch(&st->failures, 1, __ATOMIC_RELAXED);
	while (ns > max && !__atomic_compare_exchange_n(&st->max_ns, &max, ns, true,
							  __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void stat_print(const char *name, const struct request_stat *st)
{
	if (!st->count)
		return;
	fprintf(stderr, "libdrm-mlg: %-24s %10llu calls %12.3f ms total %9.1f us mean %10.1f us max%s\n",
		name, (unsigned long long)st->count, st->ns / 1e6, st->ns / 1e3 / st->count,
		st->max_ns / 1e3, st->failures ? " (some failed)" : "");
}

static void stats_print(void)
{
	char other[32];

	fprintf(stderr, "libdrm-mlg: requests to the driver (round trip, waits included):\n");
	for (unsigned int nr = 0; nr < 256; ++nr) {
		const char *name = request_name(nr);

		if (!name) {
			snprintf(other, sizeof(other), "request 0x%02x", nr);
			name = other;
		}
		stat_print(nr == 0xc3 || nr == 0xca ? (nr == 0xc3 ? "SYNCOBJ_WAIT (may sleep)" :
			   "SYNCOBJ_TIMELINE_WAIT (may sleep)") : name, &request_stats[nr]);
	}
	stat_print("SYNCOBJ_WAIT (polls)", &wait_poll_stat);
	stat_print("SYNCOBJ_TIMELINE_WAIT (polls)", &timeline_wait_poll_stat);
	stat_print("mmap", &mmap_stat);
	stat_print("munmap", &munmap_stat);
}

static bool stats_on(void)
{
	if (stats_enabled < 0) {
		const char *v = getenv("LIBDRM_MLG_STATS");

		stats_enabled = v && *v && *v != '0';
		if (stats_enabled)
			atexit(stats_print);
	}
	return stats_enabled;
}

static struct request_stat *stat_slot(unsigned long req, const void *arg)
{
	if (req == DRM_IOCTL_SYNCOBJ_WAIT &&
	    !((const struct drm_syncobj_wait *)arg)->timeout_nsec)
		return &wait_poll_stat;
	if (req == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT &&
	    !((const struct drm_syncobj_timeline_wait *)arg)->timeout_nsec)
		return &timeline_wait_poll_stat;
	return &request_stats[req & 0xff];
}

int drmIoctl(int fd, unsigned long req, void *arg)
{
	uint64_t start;
	int r;

	if (!stats_on())
		return report("request", req, drm_ioctl_fd(fd, req, arg));
	start = now_ns();
	r = drm_ioctl_fd(fd, req, arg);
	stat_add(stat_slot(req, arg), now_ns() - start, r != 0);
	return report("request", req, r);
}

static int drm_ioctl_fd(int fd, unsigned long req, void *arg)
{
	const int dfd = drm_file_driver_fd(fd);
	int r;

	if (dfd < 0)
		return -1;
	if ((uint32_t)req == DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE) {
		/* Which handle the driver returns depends on the input the
		 * output overwrites: remember it. */
		union drm_amdgpu_fence_to_handle *f = arg;
		const uint32_t what = f->in.what;

		do {
			r = request(dfd, req, arg);
		} while (r == -1 && (errno == EINTR || errno == EAGAIN));
		if (!r && (what == AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ_FD ||
			   what == AMDGPU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD)) {
			const int proxy = drm_file_wrap((int)f->out.handle, true);

			if (proxy < 0)
				return -1;
			f->out.handle = (uint32_t)proxy;
		}
		return r;
	}
	do {
		r = request(dfd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

int drmCommandNone(int fd, unsigned long index)
{
	unsigned long req = DRM_IO(DRM_COMMAND_BASE + index);

	return drmIoctl(fd, req, NULL) ? -errno : 0;
}

int drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
	unsigned long req = DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);

	return drmIoctl(fd, req, data) ? -errno : 0;
}

int drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	unsigned long req = DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, DRM_COMMAND_BASE + index, size);

	return drmIoctl(fd, req, data) ? -errno : 0;
}

int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	unsigned long req = DRM_IOC(DRM_IOC_READ | DRM_IOC_WRITE, DRM_IOCTL_BASE,
				    DRM_COMMAND_BASE + index, size);

	return drmIoctl(fd, req, data) ? -errno : 0;
}

/* ---- version, caps ---- */

drmVersionPtr drmGetVersion(int fd)
{
	struct drm_version v;
	drmVersionPtr out;

	memclear(v);
	if (drmIoctl(fd, DRM_IOCTL_VERSION, &v))
		return NULL;
	out = calloc(1, sizeof(*out));
	if (!out)
		return NULL;
	out->name = calloc(1, v.name_len + 1);
	out->date = calloc(1, v.date_len + 1);
	out->desc = calloc(1, v.desc_len + 1);
	if (!out->name || !out->date || !out->desc)
		goto fail;
	v.name = out->name;
	v.date = out->date;
	v.desc = out->desc;
	if (drmIoctl(fd, DRM_IOCTL_VERSION, &v))
		goto fail;
	out->version_major = v.version_major;
	out->version_minor = v.version_minor;
	out->version_patchlevel = v.version_patchlevel;
	out->name_len = (int)v.name_len;
	out->date_len = (int)v.date_len;
	out->desc_len = (int)v.desc_len;
	return out;
fail:
	drmFreeVersion(out);
	return NULL;
}

void drmFreeVersion(drmVersionPtr v)
{
	if (!v)
		return;
	free(v->name);
	free(v->date);
	free(v->desc);
	free(v);
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	struct drm_get_cap cap;
	int r;

	memclear(cap);
	cap.capability = capability;
	r = drmIoctl(fd, DRM_IOCTL_GET_CAP, &cap);
	if (r)
		return r;
	*value = cap.value;
	return 0;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
	struct drm_set_client_cap cap;

	memclear(cap);
	cap.capability = capability;
	cap.value = value;
	return drmIoctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
}

/* No authentication: render nodes need none, and primary-node files here
 * are never DRM master, so there is no master to authenticate with. */
int drmGetMagic(int fd, drm_magic_t *magic)
{
	(void)fd;
	*magic = 0;
	errno = EACCES;
	return -EACCES;
}

int drmAuthMagic(int fd, drm_magic_t magic)
{
	(void)fd;
	(void)magic;
	errno = EACCES;
	return -EACCES;
}

int drmIsMaster(int fd)
{
	(void)fd;
	return 0;
}

int drmAvailable(void)
{
	struct mlg_pci_identity id;

	return mlg_pci_identity(&id) == 0;
}

void drmFree(void *pt)
{
	free(pt);
}

void drmMsg(const char *format, ...)
{
	va_list ap;

	if (!getenv("LIBGL_DEBUG"))
		return;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

/* ---- devices ---- */

static int render_minor_of_path(const char *path);

/* The node a path names: DRM_NODE_RENDER, DRM_NODE_PRIMARY or -1. */
static int node_of_path(const char *path)
{
	static const char card[] = DRM_DIR_NAME "/" DRM_PRIMARY_MINOR_NAME;
	char *end = NULL;
	long minor;

	if (path && !strncmp(path, card, sizeof(card) - 1)) {
		minor = strtol(path + sizeof(card) - 1, &end, 10);
		if (end != path + sizeof(card) - 1 && !*end && minor == DRM_MLG_PRIMARY_MINOR)
			return DRM_NODE_PRIMARY;
		return -1;
	}
	return render_minor_of_path(path) < 0 ? -1 : DRM_NODE_RENDER;
}

static int render_minor_of_path(const char *path)
{
	static const char prefix[] = DRM_DIR_NAME "/" DRM_RENDER_MINOR_NAME;
	char *end = NULL;
	long minor;

	if (!path || strncmp(path, prefix, sizeof(prefix) - 1))
		return -1;
	minor = strtol(path + sizeof(prefix) - 1, &end, 10);
	if (end == path + sizeof(prefix) - 1 || *end || minor != DRM_MLG_RENDER_MINOR)
		return -1;
	return (int)minor;
}

static bool is_drm_path(const char *path)
{
	return path && !strncmp(path, DRM_DIR_NAME "/", sizeof(DRM_DIR_NAME));
}

/* The GPU as libdrm describes a PCI device: one allocation, freed by
 * drmFreeDevice. */
static int make_device(drmDevicePtr *out)
{
	struct mlg_pci_identity id;
	struct {
		drmDevice dev;
		char *nodes[DRM_NODE_MAX];
		drmPciBusInfo bus;
		drmPciDeviceInfo info;
		char render[sizeof(DRM_MLG_RENDER_PATH)];
		char primary[sizeof(DRM_MLG_PRIMARY_PATH)];
	} *d;

	if (mlg_pci_identity(&id))
		return -errno;
	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;
	memcpy(d->render, DRM_MLG_RENDER_PATH, sizeof(d->render));
	d->nodes[DRM_NODE_RENDER] = d->render;
	memcpy(d->primary, DRM_MLG_PRIMARY_PATH, sizeof(d->primary));
	d->nodes[DRM_NODE_PRIMARY] = d->primary;
	d->bus = (drmPciBusInfo){ .domain = id.domain, .bus = id.bus, .dev = id.dev,
				  .func = id.func };
	d->info = (drmPciDeviceInfo){ .vendor_id = id.vendor_id, .device_id = id.device_id,
				      .subvendor_id = id.subvendor_id,
				      .subdevice_id = id.subdevice_id,
				      .revision_id = id.revision_id };
	d->dev.nodes = d->nodes;
	d->dev.available_nodes = 1 << DRM_NODE_RENDER | 1 << DRM_NODE_PRIMARY;
	d->dev.bustype = DRM_BUS_PCI;
	d->dev.businfo.pci = &d->bus;
	d->dev.deviceinfo.pci = &d->info;
	*out = &d->dev;
	return 0;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	drmDevicePtr dev;

	(void)flags;
	if (make_device(&dev))
		return 0;	/* no driver: no devices */
	if (!devices || max_devices < 1) {
		drmFreeDevice(&dev);
		return 1;
	}
	devices[0] = dev;
	return 1;
}

int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return drmGetDevices2(DRM_DEVICE_GET_PCI_REVISION, devices, max_devices);
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	if (!device)
		return -EINVAL;
	if (!drm_file_is_node(fd))
		return -ENODEV;
	return make_device(device);
}

int drmGetDevice(int fd, drmDevicePtr *device)
{
	return drmGetDevice2(fd, DRM_DEVICE_GET_PCI_REVISION, device);
}

int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	if (!device)
		return -EINVAL;
	if (major(dev_id) != DRM_MLG_MAJOR || (minor(dev_id) != DRM_MLG_RENDER_MINOR &&
					       minor(dev_id) != DRM_MLG_PRIMARY_MINOR))
		return -ENODEV;
	return make_device(device);
}

void drmFreeDevice(drmDevicePtr *device)
{
	if (!device)
		return;
	free(*device);
	*device = NULL;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
	if (!devices)
		return;
	for (int i = 0; i < count; ++i)
		drmFreeDevice(&devices[i]);
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	if (!a || !b || a->bustype != b->bustype || a->bustype != DRM_BUS_PCI)
		return 0;
	return !memcmp(a->businfo.pci, b->businfo.pci, sizeof(drmPciBusInfo));
}

int drmGetNodeTypeFromFd(int fd)
{
	const int type = drm_file_node_type(fd);

	if (type < 0) {
		errno = EBADF;
		return -1;
	}
	return type;
}

int drmGetNodeTypeFromDevId(dev_t devid)
{
	if (major(devid) != DRM_MLG_MAJOR)
		return -ENODEV;
	if (minor(devid) == DRM_MLG_RENDER_MINOR)
		return DRM_NODE_RENDER;
	if (minor(devid) == DRM_MLG_PRIMARY_MINOR)
		return DRM_NODE_PRIMARY;
	return -ENODEV;
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
	return drm_file_is_node(fd) ? strdup(DRM_MLG_RENDER_PATH) : NULL;
}

char *drmGetDeviceNameFromFd2(int fd)
{
	const int type = drm_file_node_type(fd);

	return type == DRM_NODE_PRIMARY ? strdup(DRM_MLG_PRIMARY_PATH) :
	       type == DRM_NODE_RENDER ? strdup(DRM_MLG_RENDER_PATH) : NULL;
}

char *drmGetDeviceNameFromFd(int fd)
{
	return drmGetDeviceNameFromFd2(fd);
}

char *drmGetPrimaryDeviceNameFromFd(int fd)
{
	return drm_file_is_node(fd) ? strdup(DRM_MLG_PRIMARY_PATH) : NULL;
}

/* ---- buffers ---- */

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	struct drm_prime_handle args;
	int r;

	memclear(args);
	args.fd = -1;
	args.handle = handle;
	args.flags = flags;
	r = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	if (r)
		return r;
	*prime_fd = args.fd;
	return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	struct drm_prime_handle args;
	int r;

	memclear(args);
	args.fd = prime_fd;
	r = drmIoctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args);
	if (r)
		return r;
	*handle = args.handle;
	return 0;
}

int drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close args;

	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/* ---- syncobjs ---- */

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create args;
	int r;

	memclear(args);
	args.flags = flags;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);
	if (r)
		return r;
	*handle = args.handle;
	return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy args;

	memclear(args);
	args.handle = handle;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	struct drm_syncobj_handle args;
	int r;

	memclear(args);
	args.fd = -1;
	args.handle = handle;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args);
	if (r)
		return r;
	*obj_fd = args.fd;
	return 0;
}

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	struct drm_syncobj_handle args;
	int r;

	memclear(args);
	args.fd = obj_fd;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args);
	if (r)
		return r;
	*handle = args.handle;
	return 0;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	struct drm_syncobj_handle args;

	memclear(args);
	args.fd = sync_file_fd;
	args.handle = handle;
	args.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args);
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	struct drm_syncobj_handle args;
	int r;

	memclear(args);
	args.fd = -1;
	args.handle = handle;
	args.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args);
	if (r)
		return r;
	*sync_file_fd = args.fd;
	return 0;
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
		   unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_wait args;
	int r;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.timeout_nsec = timeout_nsec;
	args.count_handles = num_handles;
	args.flags = flags;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
	if (r < 0)
		return -errno;
	if (first_signaled)
		*first_signaled = args.first_signaled;
	return r;
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	struct drm_syncobj_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
			     uint32_t handle_count)
{
	struct drm_syncobj_timeline_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
			   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait args;
	int r;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.timeout_nsec = timeout_nsec;
	args.count_handles = num_handles;
	args.flags = flags;
	r = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
	if (r < 0)
		return -errno;
	if (first_signaled)
		*first_signaled = args.first_signaled;
	return r;
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
		     uint32_t flags)
{
	struct drm_syncobj_timeline_array args;

	memclear(args);
	args.handles = (uintptr_t)handles;
	args.points = (uintptr_t)points;
	args.count_handles = handle_count;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
		       uint64_t src_point, uint32_t flags)
{
	struct drm_syncobj_transfer args;

	memclear(args);
	args.src_handle = src_handle;
	args.dst_handle = dst_handle;
	args.src_point = src_point;
	args.dst_point = dst_point;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}

int drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd, uint32_t flags)
{
	struct drm_syncobj_eventfd args;

	memclear(args);
	args.handle = handle;
	args.point = point;
	args.fd = ev_fd;
	args.flags = flags;
	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &args);
}

/* ---- format modifiers ---- */

char *drmGetFormatModifierVendor(uint64_t modifier)
{
	switch (modifier >> 56) {
	case 0: return strdup("NONE");
	case 0x02: return strdup("AMD");
	default: return NULL;
	}
}

char *drmGetFormatModifierName(uint64_t modifier)
{
	if (modifier == 0)
		return strdup("LINEAR");
	if (modifier == 0x00ffffffffffffffull)
		return strdup("INVALID");
	return NULL;
}

/* ---- DRM files ---- */

int drmFileOpen(const char *path, int flags, ...)
{
	int dfd, fd, node;

	if (!is_drm_path(path)) {
		mode_t mode = 0;

		if (flags & O_CREAT) {
			va_list ap;

			va_start(ap, flags);
			mode = (mode_t)va_arg(ap, int);
			va_end(ap);
		}
		return open(path, flags, mode);
	}
	node = node_of_path(path);
	if (node < 0) {
		/* Only the render node and the primary node exist. */
		errno = ENOENT;
		return -1;
	}
	dfd = mlg_open(node == DRM_NODE_PRIMARY ? DRM_MLG_PRIMARY_PATH : DRM_MLG_RENDER_PATH,
		       flags & (O_ACCMODE | O_CLOEXEC | O_NONBLOCK));
	if (dfd < 0)
		return -1;
	fd = drm_file_wrap(dfd, !!(flags & O_CLOEXEC));
	if (fd >= 0)
		drm_file_set_node(fd, node);
	return fd;
}

int drmFileStat(const char *path, struct stat *st)
{
	if (!is_drm_path(path))
		return stat(path, st);
	const int node = node_of_path(path);

	if (node < 0 || !drmAvailable()) {
		errno = ENOENT;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0666;
	st->st_rdev = makedev(DRM_MLG_MAJOR, node == DRM_NODE_PRIMARY ? DRM_MLG_PRIMARY_MINOR :
			      DRM_MLG_RENDER_MINOR);
	st->st_nlink = 1;
	return 0;
}

void *drmFileMmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	int dfd;

	void *p;

	if (fd < 0 || (dfd = drm_file_driver_fd(fd)) < 0)
		return mmap(addr, length, prot, flags, fd, offset);
	if (stats_on()) {
		const uint64_t start = now_ns();

		p = mlg_mmap(addr, length, prot, flags, dfd, offset);
		stat_add(&mmap_stat, now_ns() - start, p == MAP_FAILED);
	} else {
		p = mlg_mmap(addr, length, prot, flags, dfd, offset);
	}
	if (p == MAP_FAILED)
		report("mmap at offset", (unsigned long)offset, -1);
	return p;
}

int drmFileMunmap(void *addr, size_t length)
{
	if (mlg_is_mapping(addr, length)) {
		uint64_t start;
		int r;

		if (!stats_on())
			return mlg_munmap(addr, length);
		start = now_ns();
		r = mlg_munmap(addr, length);
		stat_add(&munmap_stat, now_ns() - start, r != 0);
		return r;
	}
	return munmap(addr, length);
}
