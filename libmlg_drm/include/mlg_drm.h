/* libmlg_drm: the GPU's Linux character devices for a macOS process.
 *
 * mac_linuxgpu runs the Linux amdgpu and amdkfd drivers inside its
 * DriverKit extension, and gives each client process a Linux process of
 * its own there. These calls are open(2), close(2), ioctl(2), mmap(2) and
 * munmap(2) on that process's files: the GPU's DRM render node
 * ("/dev/dri/renderD128"), its primary node ("/dev/dri/card0", for the
 * KMS queries and framebuffers of mlg_scanout; never DRM master) and
 * "/dev/kfd". A Mesa winsys (RADV's ac_drm
 * layer) or a libdrm shim calls them where it would call the system calls
 * on Linux.
 *
 * Requests and structures are the Linux ones: ioctl numbers in Linux
 * encoding (the DRM_IOCTL_* and AMDKFD_IOC_* values a Linux build of
 * libdrm or Mesa computes; mlg_ioctl_from_bsd converts the encoding a
 * macOS build of the same headers produces) and argument structures as in
 * the Linux uapi. Each request's memory, including what its pointers
 * reach (CS chunks and BO lists, syncobj arrays, AMDGPU_INFO results,
 * version strings, KFD arrays), is carried to the driver and back by the
 * library; it knows the DRM core, amdgpu and KFD requests a Vulkan or
 * compute runtime issues and refuses others with ENOTTY.
 *
 * Errors follow the system calls: -1 (MAP_FAILED for mlg_mmap) with errno
 * set, translated from the driver's Linux errno to this platform's
 * (mlg_last_linux_errno() has the original). Requests that wait (WAIT_CS,
 * WAIT_FENCES, GEM_WAIT_IDLE, SYNCOBJ_WAIT, SYNCOBJ_TIMELINE_WAIT, KFD
 * WAIT_EVENTS) wait in the driver without holding the process's other
 * requests up, and an interrupted wait fails with EINTR so the caller
 * restarts it, as libdrm's drmIoctl does. Their absolute deadlines are
 * CLOCK_MONOTONIC times, as on Linux.
 *
 * Descriptors are in the driver process's table: the numbers are not
 * descriptors of this process and only these calls take them. Descriptors
 * a request returns (syncobj and dma-buf file descriptors) belong to the
 * same table.
 *
 * Thread-safe. The connection to the driver opens on first use. */
#ifndef MLG_DRM_H
#define MLG_DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int mlg_open(const char *path, int flags);
int mlg_close(int fd);
int mlg_ioctl(int fd, unsigned long request, void *arg);
void *mlg_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int mlg_munmap(void *addr, size_t length);
/* Whether [@addr, @addr + @length) is a mapping mlg_mmap made (and
 * mlg_munmap is the call that undoes it). */
int mlg_is_mapping(const void *addr, size_t length);

/* A request number in BSD encoding (what <sys/ioccom.h> builds, as a
 * macOS build of the DRM headers does) in Linux encoding. */
unsigned long mlg_ioctl_from_bsd(unsigned long request);
/* This platform's errno for a Linux errno (EIO for one it lacks). */
int mlg_errno_from_linux(int linux_errno);
/* The Linux errno of this thread's last failed call. */
int mlg_last_linux_errno(void);

/* The PCI function of the GPU the render node belongs to, as Linux shows
 * it in sysfs (/sys/dev/char/226:128/device): what libdrm's device
 * enumeration reports. Location fields the platform does not expose are
 * zero. */
struct mlg_pci_identity {
	uint16_t domain;
	uint8_t bus, dev, func;
	uint16_t vendor_id, device_id;
	uint16_t subvendor_id, subdevice_id;
	uint8_t revision_id;
};
/* 0, or -1 with errno set (ENODEV when there is no driver). Asks the
 * transport; a transport that cannot tell gets the vendor, device and
 * revision from the driver's AMDGPU_INFO_DEV_INFO. */
int mlg_pci_identity(struct mlg_pci_identity *out);

/* rt/lx_abi.h's LX_SCANOUT structures (include it to use these). */
struct mlg_lx_scanout;
struct mlg_lx_scanout_state;

/* The display output: show framebuffers of a primary-node descriptor
 * ("/dev/dri/card0", never DRM master) on it (LX_SCANOUT in rt/lx_abi.h;
 * @req->fd is a descriptor of this library). 0 with *state filled, or -1
 * with errno set (*state is filled as far as the driver got). */
int mlg_scanout(const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state);

/* ---- transports ----
 * The default transport is the DriverKit extension's IOKit user client
 * (type 2, MLG_USER_CLIENT_LINUX_FILE in rt/lx_abi.h). Tests and other
 * hosts install their own before the first call. Every function returns
 * a Linux errno, negated, when the call did not reach the driver. */
struct mlg_transport {
	void *ctx;
	/* open: the descriptor or -errno (the driver's own result). */
	int (*open)(void *ctx, uint32_t dev, uint32_t linux_flags);
	int (*close)(void *ctx, int fd);
	/* One request frame (rt/lx_abi.h). @async: the request waits. On 0,
	 * *result is the ioctl's return value and the reply frame fills
	 * @rbuf, *reply_bytes long. */
	int (*ioctl)(void *ctx, int fd, uint32_t cmd, const void *frame, size_t frame_bytes,
		     void *rbuf, size_t rbuf_cap, size_t *reply_bytes, int64_t *result,
		     int async);
	/* Map @length bytes at @offset of @fd into this process: *addr and an
	 * opaque *handle for unmap. */
	int (*mmap)(void *ctx, int fd, uint64_t offset, uint64_t length, uint32_t linux_prot,
		    uint32_t linux_flags, void **addr, uint64_t *handle);
	int (*munmap)(void *ctx, uint64_t handle, void *addr, uint64_t length);
	/* Optional: the GPU's PCI identity (0 or -errno). */
	int (*identity)(void *ctx, struct mlg_pci_identity *out);
	/* Optional: LX_SCANOUT. On 0, *result is the driver's 0 or -errno
	 * and *state is filled. */
	int (*scanout)(void *ctx, const struct mlg_lx_scanout *req,
		       struct mlg_lx_scanout_state *state, int64_t *result);
	/* Optional: nonzero once the driver said the device is gone for this
	 * process (the GPU left, Disconnect GPU closed its session, the driver
	 * stopped). Every mapping is then replaced by host memory, as Linux
	 * revokes a process's mmaps of an unplugged device: a store through
	 * one, anywhere in the process, never reaches a GPU that left. */
	int (*gone)(void *ctx);
};
/* Install @t (copied) for every later call; NULL restores the default.
 * Returns -1 with EBUSY once descriptors are open. */
int mlg_drm_set_transport(const struct mlg_transport *t);

#ifdef __cplusplus
}
#endif
#endif
