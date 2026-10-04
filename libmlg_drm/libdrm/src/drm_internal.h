/* libdrm for mac_linuxgpu: internals shared by the core and amdgpu parts. */
#ifndef MLG_LIBDRM_INTERNAL_H
#define MLG_LIBDRM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* ---- DRM files (drm_file.c) ----
 *
 * A driver file (a descriptor of the driver's Linux process, from
 * libmlg_drm) is represented here by one end of a socket pair: the
 * "proxy", handed to the caller as the DRM descriptor, while this library
 * keeps the other end. Copies of the proxy (dup, fork) share the socket;
 * when the last one is closed the kept end reads end-of-file, and a
 * watcher thread closes the driver file. A proxy is recognized by the
 * inode of its socket, so a copy made with dup(2) is recognized too. */

/* Wrap @driver_fd in a new proxy (close-on-exec with @cloexec). Returns the
 * proxy, or -1 with errno set after closing @driver_fd. */
int drm_file_wrap(int driver_fd, bool cloexec);
/* The driver file behind proxy @fd, or -1 with errno EBADF when @fd is not
 * a DRM file of this library. */
int drm_file_driver_fd(int fd);
/* Whether @fd is a proxy for an opened device node (not a syncobj,
 * sync_file or dma-buf descriptor). */
bool drm_file_is_node(int fd);
/* The node type of proxy @fd (DRM_NODE_PRIMARY or DRM_NODE_RENDER), or -1
 * when it is not an opened device node. */
int drm_file_node_type(int fd);
/* Mark proxy @fd as an opened device node of @type (DRM_NODE_*). */
void drm_file_set_node(int fd, int type);
/* The driver file of some opened device node, a render node first, or -1
 * when none is open. It may be released at any time by a concurrent close;
 * requests on it then fail with EBADF. */
int drm_file_any_node(void);

/* The nodes every descriptor of this library belongs to: one GPU per
 * driver. The primary node is never DRM master (the driver's display
 * output does the modesetting); it carries the KMS queries and the
 * framebuffers a client shows with drmMlgScanout. */
#define DRM_MLG_RENDER_MINOR	128
#define DRM_MLG_RENDER_PATH	"/dev/dri/renderD128"
#define DRM_MLG_PRIMARY_MINOR	0
#define DRM_MLG_PRIMARY_PATH	"/dev/dri/card0"
#define DRM_MLG_MAJOR		226

/* Linux open(2) flag values the driver decodes (DRM_CLOEXEC, DRM_RDWR). */
#define DRM_MLG_LINUX_O_RDWR	02u
#define DRM_MLG_LINUX_O_CLOEXEC	02000000u

#endif
