/* The "Linux file" RPC: the wire ABI between a client process on macOS
 * (libmlg_drm) and the per-client Linux process the dext runs for it
 * (linuxu/src/amdgpu-rt/lx_files.c, rt/lx_files.h).
 *
 * A client opens character devices (the GPU's DRM render node, /dev/kfd)
 * into its Linux process and issues ioctl(2), mmap(2), munmap(2) and
 * close(2) on them. The dext runs each call through the file's own
 * file_operations, inside the process, exactly as Linux would for a
 * process that made the system call.
 *
 * ioctl arguments. A Linux ioctl passes one integer (usually a pointer to
 * an argument block); the kernel copies the block, and whatever it points
 * to, with copy_{from,to}_user. Here the client describes every byte range
 * of its memory the ioctl may touch as a segment: the client address, the
 * length, and whether the kernel reads it (IN), writes it (OUT) or both.
 * The dext maps each segment's pages at the same addresses in the Linux
 * process's address space for the duration of the call, with the IN bytes
 * filled in and every other byte zero. Unmodified upstream code then reads
 * and writes the very addresses the client passed, nested pointers
 * included, and an address the client did not describe faults (-EFAULT),
 * as an unmapped address does on Linux. After the call the OUT bytes go
 * back to the client in request order.
 *
 * Request frame (all little endian, natural alignment):
 *
 *   struct mlg_lx_frame          header
 *   struct mlg_lx_segment[nsegs] segment table, sorted by va, disjoint
 *   uint8_t payload[]            IN bytes of each IN segment, in table
 *                                order, each at its data_offset
 *
 * Reply frame:
 *
 *   struct mlg_lx_reply          header
 *   uint8_t payload[]            OUT bytes of each OUT segment, in table
 *                                order, concatenated
 *
 * Timeouts. The DRM wait ioctls take absolute CLOCK_MONOTONIC deadlines.
 * The two clocks differ, so a client converts a deadline into the time
 * left (MLG_LX_FRAME_TIMEOUT, timeout_va, timeout_ns) and the dext writes
 * its own clock plus that time into the 8 bytes at timeout_va (inside an
 * IN segment) before the call.
 *
 * Errors are Linux errnos; a client translates them for its own platform.
 *
 * Shared with DriverKit, C++ and client builds: no kernel types. */
#ifndef LINUXU_RT_LX_ABI_H
#define LINUXU_RT_LX_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User client type of a Linux-file client (IOServiceOpen type). Types 0
 * (session) and 1 (observer) are in dext/sources/session_state.h. */
#define MLG_USER_CLIENT_LINUX_FILE	2u

/* Selectors, outside the MacAMDGPU (0-73) and observer read (80-82)
 * ranges. Every selector returns kIOReturnSuccess when the call reached
 * the Linux process; scalar output [0] is then the Linux result (a value
 * >= 0 or a negative errno, sign-extended). Any other IOReturn is a
 * transport failure: the call did not run.
 *
 * LX_OPEN    in:  [0] MLG_LX_DEV_*, [1] Linux open flags (O_RDWR,
 *                 O_CLOEXEC, O_NONBLOCK; others are refused)
 *            out: [0] the descriptor or -errno
 * LX_CLOSE   in:  [0] fd                     out: [0] 0 or -errno
 * LX_IOCTL   in:  [0] fd, [1] cmd; struct in: the request frame
 *                 (structure input, or an input descriptor above 4096
 *                 bytes)
 *            out: [0] the ioctl's result, [1] reply bytes; struct out:
 *                 the reply frame (the client sizes its buffer to
 *                 mlg_lx_reply_bytes() of its request)
 * LX_IOCTL_ASYNC  as LX_IOCTL, called with IOConnectCallAsync*: the call
 *                 runs on a worker of the client's process and returns at
 *                 once with out [0] 0 or -errno (not started), [1] its
 *                 token. Completion: async data [0] token, [1] the ioctl's
 *                 result, [2] reply bytes, [3...] the reply frame when it
 *                 fits MLG_LX_ASYNC_INLINE_BYTES, else LX_RESULT fetches
 *                 it. Used for calls that block (waits).
 * LX_RESULT  in:  [0] token; struct out: the reply frame
 *            out: [0] the ioctl's result, [1] reply bytes
 * LX_MMAP    in:  [0] fd, [1] offset (bytes, page aligned), [2] length,
 *                 [3] PROT_* (Linux values), [4] MAP_SHARED (only)
 *            out: [0] 0 or -errno, [1] memory type for
 *                 IOConnectMapMemory64, [2] mapped length
 * LX_MMAP_COMMIT  in: [0] memory type, [1] the client address the type
 *                 was mapped at                out: [0] 0 or -errno
 *                 (places the mapping in the Linux process's address
 *                 space at that address; optional)
 * LX_MUNMAP  in:  [0] memory type              out: [0] 0 or -errno
 *                 (after IOConnectUnmapMemory64)
 * LX_SCANOUT struct in: struct mlg_lx_scanout; out: [0] 0 or -errno;
 *                 struct out: struct mlg_lx_scanout_state (below) */
#define MLG_SELECTOR_LX_OPEN		96u
#define MLG_SELECTOR_LX_CLOSE		97u
#define MLG_SELECTOR_LX_IOCTL		98u
#define MLG_SELECTOR_LX_IOCTL_ASYNC	99u
#define MLG_SELECTOR_LX_RESULT		100u
#define MLG_SELECTOR_LX_MMAP		101u
#define MLG_SELECTOR_LX_MMAP_COMMIT	102u
#define MLG_SELECTOR_LX_MUNMAP		103u
#define MLG_SELECTOR_LX_SCANOUT		104u

/* LX_OPEN devices. */
#define MLG_LX_DEV_RENDER	1u	/* the GPU's DRM render node */
#define MLG_LX_DEV_KFD		2u	/* /dev/kfd */
/* The GPU's DRM primary node, never as DRM master: the driver keeps
 * modesetting to itself (its display output, docs/macos-displays.md). A
 * client reads the KMS objects, imports buffers and creates framebuffers
 * on it, and shows them through LX_SCANOUT. */
#define MLG_LX_DEV_PRIMARY	3u

/* ---- LX_SCANOUT: a client's framebuffer on the display output ----
 *
 * The driver's display output (the monitor a display agent mirrors the
 * macOS desktop to) commits every frame itself. A client attaches to it
 * and presents framebuffers it created on its primary-node file (ADDFB2);
 * the output's worker puts each on a plane in its next commit, on vblank,
 * with no copy:
 *
 *   MLG_LX_LAYER_PRIMARY  the client's image replaces the desktop on the
 *                         primary plane (fullscreen). Desktop frames that
 *                         arrive meanwhile wait, newest only, and are
 *                         shown once the client leaves the primary plane.
 *   MLG_LX_LAYER_OVERLAY  the image is on an overlay plane at dst over the
 *                         mirrored desktop (a window).
 *
 * ATTACH   connector: the output's connector name ("DP-1"), or empty for
 *          whichever connector the output drives. -ENOENT when there is
 *          no output (or not on that connector), -EBUSY when another
 *          client is attached. Reserves an overlay plane of the output's
 *          CRTC when one is free (state.overlay_plane_id, 0 if none).
 * TEST     an atomic check of fd's framebuffer fb_id on layer with
 *          src/dst, nothing committed: 0 or the driver's -errno.
 * PRESENT  queue fb_id (a framebuffer of fd's file) on layer with
 *          src/dst. syncobj (a handle of fd's file) is reset now and gets
 *          the flip's fence when the frame is committed: it signals at the
 *          vblank that shows the frame. -EBUSY while the previous present
 *          is not committed yet; the output's worker error otherwise.
 * DETACH   the desktop back on the primary plane, the overlay off; returns
 *          once that flip happened. Closing the client does the same.
 * STATE    the output and this client's attachment.
 *
 * src is in framebuffer pixels, dst in CRTC pixels; a zero width or height
 * means the whole framebuffer or the whole mode. */
#define MLG_LX_SCANOUT_ATTACH	1u
#define MLG_LX_SCANOUT_TEST	2u
#define MLG_LX_SCANOUT_PRESENT	3u
#define MLG_LX_SCANOUT_DETACH	4u
#define MLG_LX_SCANOUT_STATE	5u

#define MLG_LX_LAYER_PRIMARY	1u
#define MLG_LX_LAYER_OVERLAY	2u

#define MLG_LX_SCANOUT_NAME_BYTES	32u
#define MLG_LX_SCANOUT_VERSION	1u

struct mlg_lx_scanout {
	uint32_t version;	/* MLG_LX_SCANOUT_VERSION */
	uint32_t op;		/* MLG_LX_SCANOUT_* */
	int32_t fd;		/* a primary-node descriptor of the client */
	uint32_t layer;		/* MLG_LX_LAYER_* */
	uint32_t fb_id;
	uint32_t syncobj;
	uint32_t src_x, src_y, src_w, src_h;
	int32_t dst_x, dst_y;
	uint32_t dst_w, dst_h;
	char connector[MLG_LX_SCANOUT_NAME_BYTES];
	uint64_t reserved[2];	/* zero */
};

struct mlg_lx_scanout_state {
	uint32_t version;	/* MLG_LX_SCANOUT_VERSION */
	int32_t error;		/* the output's worker error, 0 */
	uint32_t output;	/* an output is running */
	uint32_t attached;	/* this client is attached */
	uint32_t connector_id, crtc_id, primary_plane_id, overlay_plane_id;
	uint32_t width, height, refresh_mhz;	/* the output's mode */
	uint32_t layer;		/* the layer this client's last present used, 0 */
	uint64_t presents;	/* this client's presents taken */
	uint64_t flips;		/* ... that reached the screen */
	uint64_t desktop_held;	/* desktop frames that waited while the client had the primary plane */
	char connector[MLG_LX_SCANOUT_NAME_BYTES];
	uint64_t reserved[2];
};
#ifdef __cplusplus
static_assert(sizeof(struct mlg_lx_scanout) == 104, "mlg_lx_scanout layout");
static_assert(sizeof(struct mlg_lx_scanout_state) == 120, "mlg_lx_scanout_state layout");
#else
_Static_assert(sizeof(struct mlg_lx_scanout) == 104, "mlg_lx_scanout layout");
_Static_assert(sizeof(struct mlg_lx_scanout_state) == 120, "mlg_lx_scanout_state layout");
#endif

/* Linux open(2)/mmap(2) flag values the transport accepts. */
#define MLG_LX_O_ACCMODE	00000003u
#define MLG_LX_O_RDWR		00000002u
#define MLG_LX_O_NONBLOCK	00004000u
#define MLG_LX_O_CLOEXEC	02000000u
#define MLG_LX_PROT_READ	0x1u
#define MLG_LX_PROT_WRITE	0x2u
#define MLG_LX_MAP_SHARED	0x01u

/* Memory types LX_MMAP hands out: MLG_LX_MMAP_TYPE_BASE + a per-client
 * index. The dext's other memory types (BARs 0-5, BO maps from 0x10000,
 * the firmware mailbox) lie below. */
#define MLG_LX_MMAP_TYPE_BASE	0x40000000ull
#define MLG_LX_MMAP_TYPE_LIMIT	0x7fffffffull

#define MLG_LX_FRAME_MAGIC	0x58474c4du	/* "MLGX" */
#define MLG_LX_REPLY_MAGIC	0x52474c4du	/* "MLGR" */
#define MLG_LX_VERSION		1u

/* Limits the dext enforces (a frame beyond them is -E2BIG). */
#define MLG_LX_MAX_SEGMENTS	256u
#define MLG_LX_MAX_SEGMENT_BYTES (4u << 20)
#define MLG_LX_MAX_FRAME_BYTES	(8u << 20)	/* request, and reply */
#define MLG_LX_MAX_ARG_PAGES	2048u		/* distinct pages a call maps */
/* Client addresses lie in [MLG_LX_VA_MIN, MLG_LX_VA_LIMIT). */
#define MLG_LX_VA_MIN		0x1000ull
#define MLG_LX_VA_LIMIT		(1ull << 47)

/* IOKit limits the transport is built around. */
#define MLG_LX_INLINE_STRUCT_BYTES	4096u
#define MLG_LX_ASYNC_WORDS		16u
#define MLG_LX_ASYNC_INLINE_BYTES	((MLG_LX_ASYNC_WORDS - 3u) * 8u)
/* Async calls in flight per client; LX_IOCTL_ASYNC beyond it is -EAGAIN.
 * Completed results not yet fetched with LX_RESULT count too. */
#define MLG_LX_MAX_ASYNC	16u

/* The Linux errno values the transport itself reports, spelled out so a
 * client build does not pick up its own platform's numbers. */
#define MLG_LX_ENOENT		2
#define MLG_LX_ESRCH		3
#define MLG_LX_EINTR		4
#define MLG_LX_E2BIG		7
#define MLG_LX_EBADF		9
#define MLG_LX_EAGAIN		11
#define MLG_LX_ENOMEM		12
#define MLG_LX_EFAULT		14
#define MLG_LX_EBUSY		16
#define MLG_LX_ENODEV		19
#define MLG_LX_EINVAL		22
#define MLG_LX_ENOTTY		25
#define MLG_LX_ENOSPC		28

/* Segment directions. */
#define MLG_LX_SEG_IN		1u
#define MLG_LX_SEG_OUT		2u
#define MLG_LX_SEG_INOUT	(MLG_LX_SEG_IN | MLG_LX_SEG_OUT)

/* Frame flags. */
#define MLG_LX_FRAME_TIMEOUT	1u	/* timeout_va/timeout_ns are valid */
#define MLG_LX_FRAME_FLAGS	MLG_LX_FRAME_TIMEOUT

struct mlg_lx_frame {
	uint32_t magic;		/* MLG_LX_FRAME_MAGIC */
	uint16_t version;	/* MLG_LX_VERSION */
	uint16_t header_bytes;	/* sizeof(struct mlg_lx_frame) */
	uint32_t total_bytes;	/* header + table + payload */
	uint32_t cmd;		/* the ioctl command, as the scalar */
	uint64_t arg;		/* the ioctl's integer argument */
	uint32_t nsegs;
	uint32_t flags;		/* MLG_LX_FRAME_* */
	uint64_t timeout_va;	/* client address of an absolute deadline */
	uint64_t timeout_ns;	/* time left until it, saturated */
	uint64_t reserved[2];	/* zero */
};

struct mlg_lx_segment {
	uint64_t va;		/* client address */
	uint32_t size;		/* bytes, 1 .. MLG_LX_MAX_SEGMENT_BYTES */
	uint32_t dir;		/* MLG_LX_SEG_* */
	uint32_t data_offset;	/* IN: offset of its bytes in the frame */
	uint32_t reserved;	/* zero */
};

struct mlg_lx_reply {
	uint32_t magic;		/* MLG_LX_REPLY_MAGIC */
	uint16_t version;
	uint16_t header_bytes;	/* sizeof(struct mlg_lx_reply) */
	uint32_t total_bytes;	/* header + OUT payload */
	uint32_t out_segments;	/* OUT segments, in request order */
	int64_t result;		/* the ioctl's return value or -errno */
};

#ifdef __cplusplus
#define MLG_LX_STATIC_ASSERT static_assert
#else
#define MLG_LX_STATIC_ASSERT _Static_assert
#endif
MLG_LX_STATIC_ASSERT(sizeof(struct mlg_lx_frame) == 64, "frame header");
MLG_LX_STATIC_ASSERT(sizeof(struct mlg_lx_segment) == 24, "segment");
MLG_LX_STATIC_ASSERT(sizeof(struct mlg_lx_reply) == 24, "reply header");
#undef MLG_LX_STATIC_ASSERT

/* Request validation (linuxu/src/amdgpu-rt/lx_frame.c, also linked by the
 * client library's tests). Checks everything about @frame that does not
 * depend on the process: header fields, segment bounds, directions, order
 * and disjointness, IN data ranges, the timeout field, the limits above.
 * On success fills the OUT byte count the reply will carry. Returns 0 or a
 * negative Linux errno (-EINVAL, -E2BIG, -EFAULT). */
int mlg_lx_frame_check(const void *frame, size_t bytes, uint32_t cmd,
		       uint64_t *out_bytes);
/* The reply size a request with @out_bytes of OUT segments produces. */
static inline uint64_t mlg_lx_reply_bytes(uint64_t out_bytes)
{
	return sizeof(struct mlg_lx_reply) + out_bytes;
}

/* ---- the client side (lx_frame.c, lx_describe.c) ----
 *
 * These run in the address space that owns the ioctl's memory: the client
 * library in a macOS process, or an in-dext client such as the CS
 * self-test. Addresses are pointers of that address space. */

/* A byte range an ioctl touches, in any order; ranges may overlap. */
struct mlg_lx_span {
	uint64_t va;
	uint64_t size;
	uint32_t dir;		/* MLG_LX_SEG_* */
	uint32_t reserved;
};

/* The most spans mlg_lx_describe produces for one call. */
#define MLG_LX_DESCRIBE_MAX	(2u * MLG_LX_MAX_SEGMENTS)

/* Whether @cmd is one the transport carries for device @dev
 * (MLG_LX_DEV_*): the DRM core and amdgpu ioctls of a render-node client,
 * the KFD ioctls of a compute runtime. The dext admits nothing else. */
int mlg_lx_cmd_known(uint32_t dev, uint32_t cmd);
/* Whether @cmd may wait (sent through LX_IOCTL_ASYNC). */
int mlg_lx_cmd_blocks(uint32_t dev, uint32_t cmd);
/* The memory ioctl @cmd with argument @arg reads and writes: its argument
 * block and every range a pointer in it (or in the ranges it points to)
 * names, read from the caller's memory. *@timeout_va is set to the address
 * of an absolute CLOCK_MONOTONIC deadline the call carries, or 0. Returns
 * 0, -ENOTTY for an unknown command, -E2BIG when the ranges exceed @cap or
 * a range exceeds MLG_LX_MAX_SEGMENT_BYTES, -EFAULT for a null pointer
 * with a nonzero length. */
int mlg_lx_describe(uint32_t dev, uint32_t cmd, uint64_t arg,
		    struct mlg_lx_span *spans, uint32_t cap, uint32_t *count,
		    uint64_t *timeout_va);

/* Build the request frame: spans are sorted and overlapping ones merged,
 * and the IN bytes copied from the caller's memory. When @timeout_va is
 * nonzero and the deadline there is a finite time (neither 0 nor
 * negative), the frame carries the time left until it, measured against
 * @now_ns on the caller's clock. With @buf NULL only sizes are computed.
 * Returns the frame size, or -errno (-E2BIG beyond the limits, -ENOMEM,
 * -ENOSPC when @cap is short). *@out_bytes gets the reply payload size. */
long mlg_lx_encode(uint32_t cmd, uint64_t arg, const struct mlg_lx_span *spans,
		   uint32_t count, uint64_t timeout_va, uint64_t now_ns,
		   void *buf, size_t cap, uint64_t *out_bytes);
/* Copy the reply's OUT bytes to the caller's memory, the segments of
 * @frame naming where. Returns 0 with *@result, or -EINVAL for a reply
 * that does not match the frame. */
int mlg_lx_apply_reply(const void *frame, size_t frame_bytes, const void *rbuf,
		       size_t reply_bytes, int64_t *result);

#ifdef __cplusplus
}
#endif
#endif
