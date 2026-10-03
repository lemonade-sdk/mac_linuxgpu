/* Cached session state shared by the user client, the host app and the
 * offline readers (scripts/read-driver-log.py keeps the same numbers).
 *
 * The session-state snapshot (QueryInfo tag DEXT_COMPUTE_QUERY_SESSION_STATE)
 * is built from lifecycle variables only: it never claims PCI, touches MMIO
 * or configuration space, or dereferences upstream device state.
 *
 *   out[0] layout version (MLG_SESSION_STATE_VERSION)
 *   out[1] MLG_SESSION_FLAG_* bits
 *   out[2] quarantine cause (MLG_QUARANTINE_*), first trigger wins
 *   out[3] cause code, sign-extended (the failing step's return value)
 *   out[4] step that observed a definite PCI fault (cause MLG_QUARANTINE_PCI_FAULT)
 *   out[5] last bus-master isolation result, sign-extended (flag ISOLATION_ATTEMPTED)
 *   out[6] release blocker (MLG_RELEASE_*); MLG_RELEASE_READY when releasable
 *   out[7] session generation
 *   out[8] session participants
 */
#ifndef MACLINUXGPU_SESSION_STATE_H
#define MACLINUXGPU_SESSION_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MLG_SESSION_STATE_VERSION 1u
#define MLG_SESSION_STATE_WORDS   9u

enum mlg_session_flag {
	MLG_SESSION_FLAG_CLOSING             = 1u << 0,
	MLG_SESSION_FLAG_QUARANTINED         = 1u << 1,
	MLG_SESSION_FLAG_STOPPING            = 1u << 2,
	MLG_SESSION_FLAG_PCI_OPEN            = 1u << 3,
	MLG_SESSION_FLAG_MODULES_RUNNING     = 1u << 4,
	MLG_SESSION_FLAG_FINAL_CLEANUP       = 1u << 5, /* interrupt drain completed */
	MLG_SESSION_FLAG_RELEASABLE          = 1u << 6,
	MLG_SESSION_FLAG_RESTART_REQUIRED    = 1u << 7,
	MLG_SESSION_FLAG_RAW_BAR_MAPPED      = 1u << 8,
	MLG_SESSION_FLAG_RUNTIME_DEVICE      = 1u << 9,
	MLG_SESSION_FLAG_ISOLATION_ATTEMPTED = 1u << 10,
};

/* Which close/probe step quarantined the session. */
enum mlg_quarantine_cause {
	MLG_QUARANTINE_NONE               = 0,
	MLG_QUARANTINE_RAW_BAR_MAPPING    = 1,  /* raw BAR mapping lifetime uncertain */
	MLG_QUARANTINE_SHUTDOWN_HOLD      = 2,  /* DMA shutdown reservation failed */
	MLG_QUARANTINE_COMPUTE_UNCERTAIN  = 3,  /* dext_compute_stop: GPU completion uncertain */
	MLG_QUARANTINE_IRQ_CANCEL         = 4,  /* interrupt cancellation failed */
	MLG_QUARANTINE_ENDPOINT_ISOLATION = 5,  /* shutdown function reset failed or was refused */
	MLG_QUARANTINE_DMA_RETAINED       = 6,  /* dext_dma_fini found retained backing */
	MLG_QUARANTINE_PCI_FAULT          = 7,  /* definite PCI transport fault; code = fault kind */
	MLG_QUARANTINE_PROBE_HOLD         = 8,  /* probe DMA reservation failed */
	MLG_QUARANTINE_PROBE_RETAINED     = 9,  /* failed-probe ownership retained */
	MLG_QUARANTINE_PROBE_COMMIT       = 10, /* probe DMA commit failed */
	MLG_QUARANTINE_CLIENT_RELEASE     = 11, /* shared-session client cleanup failed */
	MLG_QUARANTINE_RELEASE_FAILED     = 12, /* explicit release attempt failed */
};

/* Why a quarantined session cannot be released now. */
enum mlg_release_blocker {
	MLG_RELEASE_READY             = 0,
	MLG_RELEASE_NOT_QUARANTINED   = 1,
	MLG_RELEASE_IRQ_PENDING       = 2,  /* drain requested, completion not yet run */
	MLG_RELEASE_IRQ_FAILED        = 3,  /* a source still owns live callbacks */
	MLG_RELEASE_UPSTREAM_RETAINED = 4,  /* upstream driver or runtime device not removed */
	MLG_RELEASE_COMPUTE_RETAINED  = 5,  /* compute context or uncertain GPU work retained */
	MLG_RELEASE_RAW_BAR_MAPPED    = 6,  /* a client still holds a raw BAR mapping */
	MLG_RELEASE_PARTICIPANTS      = 7,  /* session clients have not all exited */
	MLG_RELEASE_DMA_OWNED         = 8,  /* live DMA owner, alias, operation or failed completion */
	MLG_RELEASE_PCI_FAULT         = 9,  /* definite PCI transport fault */
	MLG_RELEASE_PCI_BUSY          = 10, /* PCI admission has not drained */
	MLG_RELEASE_RESET_FAILED      = 11, /* a release attempt could not reset the endpoint */
};

/* Blockers that no later event in this process can clear. Only these make a
 * restart necessary; the others clear once a client exits or a drain ends. */
static inline bool mlg_release_blocker_permanent(uint32_t blocker)
{
	switch (blocker) {
	case MLG_RELEASE_IRQ_FAILED:
	case MLG_RELEASE_UPSTREAM_RETAINED:
	case MLG_RELEASE_COMPUTE_RETAINED:
	case MLG_RELEASE_DMA_OWNED:
	case MLG_RELEASE_PCI_FAULT:
	case MLG_RELEASE_RESET_FAILED:
		return true;
	default:
		return false;
	}
}

/* Observer user clients (type MLG_USER_CLIENT_OBSERVER) never join or close
 * a session. They may call selectors that read cached state, the
 * entitlement-checked release selector, the two Linux read paths below,
 * which run upstream callbacks while the driver runs and never claim PCI,
 * join the session or touch queues, and the self-contained submission
 * self-test (DrmSelfTest). Linux-file clients are type 2
 * (MLG_USER_CLIENT_LINUX_FILE, linuxu/headers/rt/lx_abi.h). */
#define MLG_USER_CLIENT_SESSION  0u
#define MLG_USER_CLIENT_OBSERVER 1u

#define MLG_SELECTOR_PING                0u
#define MLG_SELECTOR_QUERY_INFO          21u
#define MLG_SELECTOR_RUNTIME_BUILD       43u
#define MLG_SELECTOR_RELEASE_QUARANTINE  61u
/* Outside the MacAMDGPU selector range (0-73): MacLinuxGPU's Linux paths. */
#define MLG_SELECTOR_SYSFS_READ          80u
#define MLG_SELECTOR_DRM_INFO            81u
#define MLG_SELECTOR_DRM_SELFTEST        82u

/* SysfsRead: the amdgpu device's sysfs directory, read as Linux sysfs reads
 * it (the attribute's show(), or a bin_attribute's read()), or listed.
 *   scalar in:  [0] MLG_SYSFS_OP_*, [1] byte offset into the file/listing
 *   struct in:  path relative to the device directory, e.g.
 *               "gpu_metrics", "hwmon/hwmon0/temp1_input"; 1 to
 *               MLG_SYSFS_PATH_MAX bytes, '/'-separated, no empty, "." or
 *               ".." component; one trailing NUL is allowed. A listing
 *               may pass no path: the device directory itself
 *   struct out: at most MLG_SYSFS_CHUNK_MAX bytes from the offset
 *   scalar out: [0] 0 or a negative Linux errno, sign-extended (show()'s
 *               own error, ENOENT, ENOTDIR, EISDIR, EACCES, ...)
 *               [1] bytes returned
 *               [2] full length: show()'s count, the bin file's size or
 *               the listing's size (read again at an offset for more)
 * A listing is "<type> <name>\n" per entry, sorted, type f (file),
 * d (directory) or l (link). Each call runs show() afresh.
 * IOReturn: NotReady unless the upstream driver runs in an open,
 * unquarantined session; BadArgument for a malformed call.
 *
 * DrmInfo: one DRM_IOCTL_AMDGPU_INFO on a render-node file the driver holds
 * for observers, through upstream amdgpu_info_ioctl.
 *   scalar in:  [0] query (AMDGPU_INFO_SENSOR, _VRAM_USAGE, _VIS_VRAM_USAGE,
 *               _GTT_USAGE, _VRAM_GTT, _MEMORY, _DEV_INFO or _READ_MMR_REG)
 *               [1] return_size, 1 to MLG_SYSFS_CHUNK_MAX
 *   struct in:  the request's argument union (sensor_info, read_mmr_reg),
 *               at most 16 bytes, or none
 *   struct out: return_size bytes of the result
 *   scalar out: [0] 0 or the ioctl's negative Linux errno, sign-extended
 * IOReturn as SysfsRead; NotPermitted for any other query. */
/* DrmSelfTest: the kernel-queue command submission self-test
 * (linuxu/headers/rt/cs_selftest.h) on the GPU, in a Linux process of its
 * own: render node, AMDGPU_INFO, a context, GEM buffers mapped in its own
 * GPUVM, AMDGPU_CS on the compute ring and on SDMA, AMDGPU_WAIT_CS and
 * syncobj waits, its VRAM buffer moved to GTT (through a GART transfer
 * window, as TTM evicts) and back by submissions, then everything undone. It touches nothing of any other
 * client and creates no queue; each wait is bounded.
 *   scalar in:  [0] MLG_DRM_SELFTEST_CONFIRM
 *   struct out: struct rt_cs_selftest_result (MLG_DRM_SELFTEST_RESULT_MAX
 *               bytes at most; its version and steps fields describe it)
 *   scalar out: [0] 0 or the first failing step's status, sign-extended
 *               (a negative Linux errno, RT_CS_MISMATCH, RT_CS_PARKED)
 *               [1] earlier tests still waiting for their GPU work
 * A test whose work never completed keeps its process until the work
 * does (RT_CS_PARKED); the session then cannot close cleanly until it
 * has. IOReturn as DrmInfo; Busy while another test runs. */
#define MLG_DRM_SELFTEST_CONFIRM    0x43535354ULL /* "CSST" */
#define MLG_DRM_SELFTEST_WORDS      2u
#define MLG_DRM_SELFTEST_RESULT_MAX 512u

#define MLG_SYSFS_OP_READ      0u
#define MLG_SYSFS_OP_LIST      1u
#define MLG_SYSFS_PATH_MAX     256u
#define MLG_SYSFS_CHUNK_MAX    4096u
#define MLG_SYSFS_READ_WORDS   3u
#define MLG_DRM_INFO_ARGS_MAX  16u

#define MLG_QUERY_PROBE_STATUS  0x4c50524fULL /* "LPRO" */
#define MLG_QUERY_KERNEL_LOG    0x4c4c4f47ULL /* "LLOG" */
#define MLG_QUERY_SESSION_STATE 0x4c534553ULL /* "LSES" */

/* Entitlement a client must hold to release a quarantined session. */
#define MLG_SESSION_RELEASE_ENTITLEMENT "com.geramyloveless.MacAMDGPUHost.session-release"

/* SysfsRead's path argument: copy @length bytes (one trailing NUL allowed)
 * into @path as a C string if they form a relative sysfs path of at most
 * MLG_SYSFS_PATH_MAX bytes with no empty, "." or ".." component. With
 * @directory, no bytes at all name the device directory itself. */
static inline bool mlg_sysfs_path_copy(char path[MLG_SYSFS_PATH_MAX + 1],
				       const char *bytes, size_t length, bool directory)
{
	if (bytes && length && bytes[length - 1] == '\0')
		--length;
	if (directory && (!bytes || !length)) {
		path[0] = '\0';
		return true;
	}
	if (!bytes || !length || length > MLG_SYSFS_PATH_MAX)
		return false;
	size_t start = 0;
	for (size_t i = 0; i <= length; ++i) {
		if (i < length && bytes[i] == '\0')
			return false;
		if (i < length && bytes[i] != '/')
			continue;
		const size_t n = i - start;
		if (!n || (n == 1 && bytes[start] == '.') ||
		    (n == 2 && bytes[start] == '.' && bytes[start + 1] == '.'))
			return false;
		start = i + 1;
	}
	for (size_t i = 0; i < length; ++i)
		path[i] = bytes[i];
	path[length] = '\0';
	return true;
}

static inline bool mlg_observer_selector_allowed(uint64_t selector,
						 const uint64_t *input,
						 uint32_t input_count)
{
	switch (selector) {
	case MLG_SELECTOR_PING:
	case MLG_SELECTOR_RUNTIME_BUILD:
	case MLG_SELECTOR_RELEASE_QUARANTINE:
		return true;
	case MLG_SELECTOR_QUERY_INFO:
		if (!input || !input_count)
			return false;
		return (input[0] == MLG_QUERY_PROBE_STATUS && input_count == 1) ||
		       (input[0] == MLG_QUERY_SESSION_STATE && input_count == 1) ||
		       (input[0] == MLG_QUERY_KERNEL_LOG && input_count == 2);
	case MLG_SELECTOR_SYSFS_READ:
		return input && input_count == 2 &&
		       (input[0] == MLG_SYSFS_OP_READ || input[0] == MLG_SYSFS_OP_LIST);
	case MLG_SELECTOR_DRM_INFO:
		return input && input_count == 2 && input[1] &&
		       input[1] <= MLG_SYSFS_CHUNK_MAX;
	case MLG_SELECTOR_DRM_SELFTEST:
		return input && input_count == 1 && input[0] == MLG_DRM_SELFTEST_CONFIRM;
	default:
		return false;
	}
}

#endif
