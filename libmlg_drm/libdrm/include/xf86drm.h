/* libdrm for mac_linuxgpu: the part of libdrm's core API (xf86drm.h) that
 * Mesa's amdgpu drivers and Vulkan runtime use, over libmlg_drm.
 *
 * Names, types and return conventions are libdrm's, so code written for
 * libdrm builds against this header unchanged. The DRM files behind the
 * descriptors are files of the GPU driver's Linux process (mac_linuxgpu's
 * DriverKit extension), reached through libmlg_drm; see the "DRM files"
 * section at the end for what that means for code that also makes system
 * calls on them.
 *
 * Structures and request numbers come from the Linux uapi <drm.h>, built
 * for a BSD-like platform: requests are in the BSD encoding of
 * <sys/ioccom.h>, and drmIoctl converts them for the driver. */
#ifndef MLG_XF86DRM_H
#define MLG_XF86DRM_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef DRM_MAX_MINOR
#define DRM_MAX_MINOR 64
#endif

#define DRM_DIR_NAME		"/dev/dri"
#define DRM_PRIMARY_MINOR_NAME	"card"
#define DRM_RENDER_MINOR_NAME	"renderD"

#define DRM_IOCTL_NR(n)		((n) & 0xff)
#define DRM_IOC_VOID		IOC_VOID
#define DRM_IOC_READ		IOC_OUT
#define DRM_IOC_WRITE		IOC_IN
#define DRM_IOC_READWRITE	IOC_INOUT
#define DRM_IOC(dir, group, nr, size) _IOC(dir, group, nr, size)

#define DRM_ERR_NO_DEVICE	(-1001)
#define DRM_ERR_NO_ACCESS	(-1002)
#define DRM_ERR_NOT_ROOT	(-1003)
#define DRM_ERR_INVALID		(-1004)
#define DRM_ERR_NO_FD		(-1005)

typedef unsigned int drm_magic_t;
typedef void *drmAddress, **drmAddressPtr;
typedef unsigned int drmSize, *drmSizePtr;

typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

extern int drmIoctl(int fd, unsigned long request, void *arg);
extern drmVersionPtr drmGetVersion(int fd);
extern void drmFreeVersion(drmVersionPtr version);
extern int drmGetCap(int fd, uint64_t capability, uint64_t *value);
extern int drmSetClientCap(int fd, uint64_t capability, uint64_t value);
extern int drmGetMagic(int fd, drm_magic_t *magic);
extern int drmAuthMagic(int fd, drm_magic_t magic);
extern int drmIsMaster(int fd);
extern int drmAvailable(void);
extern void drmFree(void *pt);
extern void drmMsg(const char *format, ...) __attribute__((format(printf, 1, 2)));

extern int drmCommandNone(int fd, unsigned long drmCommandIndex);
extern int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data,
			  unsigned long size);
extern int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data,
			   unsigned long size);
extern int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data,
			       unsigned long size);

/* ---- nodes and devices ---- */

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI       0
#define DRM_BUS_USB       1
#define DRM_BUS_PLATFORM  2
#define DRM_BUS_HOST1X    3
#define DRM_BUS_FAUX      4

typedef struct _drmPciBusInfo {
	uint16_t domain;
	uint8_t bus;
	uint8_t dev;
	uint8_t func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subvendor_id;
	uint16_t subdevice_id;
	uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

#define DRM_PLATFORM_DEVICE_NAME_LEN 512
typedef struct _drmPlatformBusInfo {
	char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;
typedef struct _drmPlatformDeviceInfo {
	char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
	char **nodes;		/* DRM_NODE_MAX entries */
	int available_nodes;	/* DRM_NODE_* bitmask */
	int bustype;
	union {
		drmPciBusInfoPtr pci;
		drmPlatformBusInfoPtr platform;
	} businfo;
	union {
		drmPciDeviceInfoPtr pci;
		drmPlatformDeviceInfoPtr platform;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

#define DRM_DEVICE_GET_PCI_REVISION (1 << 0)

extern int drmGetDevice(int fd, drmDevicePtr *device);
extern int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
extern int drmGetDevices(drmDevicePtr devices[], int max_devices);
extern int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
extern int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device);
extern void drmFreeDevice(drmDevicePtr *device);
extern void drmFreeDevices(drmDevicePtr devices[], int count);
extern int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
extern int drmGetNodeTypeFromFd(int fd);
extern int drmGetNodeTypeFromDevId(dev_t devid);
extern char *drmGetDeviceNameFromFd(int fd);
extern char *drmGetDeviceNameFromFd2(int fd);
extern char *drmGetPrimaryDeviceNameFromFd(int fd);
extern char *drmGetRenderDeviceNameFromFd(int fd);

/* ---- buffers ---- */

#define DRM_RDWR O_RDWR
#define DRM_CLOEXEC O_CLOEXEC

extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
extern int drmCloseBufferHandle(int fd, uint32_t handle);

/* ---- syncobjs ---- */

extern int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
extern int drmSyncobjDestroy(int fd, uint32_t handle);
extern int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
extern int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
extern int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
extern int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
extern int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
			  int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
				    uint32_t handle_count);
extern int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
				  unsigned num_handles, int64_t timeout_nsec, unsigned flags,
				  uint32_t *first_signaled);
extern int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points,
			   uint32_t handle_count);
extern int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
			    uint32_t handle_count, uint32_t flags);
extern int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
			      uint32_t src_handle, uint64_t src_point, uint32_t flags);
extern int drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd,
			     uint32_t flags);

/* ---- format modifiers ---- */

extern char *drmGetFormatModifierVendor(uint64_t modifier);
extern char *drmGetFormatModifierName(uint64_t modifier);

/* ---- DRM files ----
 *
 * The device files are files of the driver's Linux process, not kernel
 * files. Each one this library hands out (an opened node, and every
 * syncobj, sync_file and dma-buf descriptor a request returns) is
 * represented in this process by a descriptor of its own, a socket:
 * close(2), dup(2), fcntl(2) and poll(2) take it as they take any
 * descriptor, and the driver's file is released once the last copy is
 * closed, as a kernel file is. Every function of this library, and
 * drmIoctl, takes these descriptors.
 *
 * What the system cannot do for such a file is open it by path, stat its
 * node or map it. LIBDRM_FILE_OPS tells portable code to use these in place
 * of open(2), stat(2), mmap(2) and munmap(2) for DRM nodes and DRM files;
 * they behave as the system calls do (-1 or MAP_FAILED with errno). Other
 * paths, descriptors and addresses are passed to the system calls. */
#define LIBDRM_FILE_OPS 1

extern int drmFileOpen(const char *path, int flags, ...);
extern int drmFileStat(const char *path, struct stat *st);
extern void *drmFileMmap(void *addr, size_t length, int prot, int flags, int fd,
			 off_t offset);
extern int drmFileMunmap(void *addr, size_t length);

/* ---- the driver's display output (mac_linuxgpu) ----
 *
 * The driver mirrors the macOS desktop to a monitor on the GPU (its
 * display output) and is the only one that commits to it. A client shows
 * its own framebuffers there instead of the desktop, or over it, with no
 * copy: framebuffers of a primary-node descriptor (drmModeAddFB2 on
 * "/dev/dri/card0", xf86drmMode.h) handed to the output's next commit. See
 * rt/lx_abi.h (LX_SCANOUT) for the operations and layers. */
#define LIBDRM_MLG_SCANOUT 1

#include <rt/lx_abi.h>

/* One LX_SCANOUT operation. @req->fd is a primary-node descriptor of this
 * library (its proxy; translated here). Returns 0 or -errno, and fills
 * *@state either way. */
extern int drmMlgScanout(const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state);

/* The macOS display (CGDirectDisplayID) that stands for the monitor on
 * connector @connector_id: the one whose vendor, model and serial number
 * are the ones the display agent gives a virtual display for that monitor's
 * EDID (host/DisplayAgent.swift). 0 with *@display_id, -ENOENT when no
 * online display matches, -EEXIST when more than one does, or -errno from
 * reading the EDID through @fd. */
extern int drmMlgConnectorDisplay(int fd, uint32_t connector_id, uint32_t *display_id);
/* The identity a display agent gives the monitor with this EDID base
 * block: the PNP vendor word, the product code and the serial number (the
 * numeric one, else an FNV-1a hash of the serial string descriptor, else
 * 0). -EINVAL for a block that is not an EDID. */
extern int drmMlgEdidIdentity(const void *edid, size_t length, uint32_t *vendor,
			      uint32_t *product, uint32_t *serial);

/* Where a window's frames go on the output (drmMlgPlaceWindow). Rectangles
 * of macOS are in global points, origin top left (CoreGraphics' space);
 * the monitor's screen covers the output's whole mode, so a point maps to
 * crtc_w / screen width pixels (the screen's backing scale and the mode in
 * one factor). */
#define DRM_MLG_PLACE_AUTO		0	/* full screen when the window fills its screen */
#define DRM_MLG_PLACE_FULLSCREEN	1
#define DRM_MLG_PLACE_WINDOWED		2
/* DC's smallest viewport (MIN_VIEWPORT_SIZE): a smaller visible part of a
 * window is not shown rather than grown by DC. */
#define DRM_MLG_MIN_VIEWPORT		12

struct drm_mlg_place_in {
	int mode;			/* DRM_MLG_PLACE_* */
	int have_screen;		/* the monitor has a macOS screen */
	double screen_x, screen_y, screen_w, screen_h;
	int have_window;		/* the layer is in a window */
	int window_native_fullscreen;	/* AppKit's full screen, on the monitor's screen */
	double win_x, win_y, win_w, win_h;	/* the window's content */
	uint32_t crtc_w, crtc_h;	/* the output's mode */
	uint32_t image_w, image_h;	/* the swapchain's images */
};

struct drm_mlg_placement {
	uint32_t layer;			/* MLG_LX_LAYER_*, or 0: nothing shown */
	uint32_t src_x, src_y, src_w, src_h;	/* in image pixels */
	int32_t dst_x, dst_y;		/* on the CRTC */
	uint32_t dst_w, dst_h;
	char why[128];			/* for layer 0 or an error: the reason */
};

/* The layer and rectangles for one frame: the primary plane, whole, for a
 * window that fills the monitor's screen (or MODE_FULLSCREEN, or no screen
 * to place it on); else an overlay plane at the window's content mapped to
 * CRTC pixels, clipped to the CRTC with the matching crop of the image;
 * nothing (layer 0, with why) when no part of the window is on the
 * monitor, or the part is under DRM_MLG_MIN_VIEWPORT pixels. Returns 0, or
 * -EINVAL with why for inputs that make no placement. */
extern int drmMlgPlaceWindow(const struct drm_mlg_place_in *in, struct drm_mlg_placement *out);

#if defined(__cplusplus)
}
#endif

#endif
