/* rt/fw_mailbox.h - on-demand firmware transport between the dext and a
 * user-space firmware servicer.
 *
 * A DriverKit dext cannot open files, so request_firmware() cannot read
 * /lib/firmware the way Linux does.  Instead the dext owns one shared
 * memory region (exported to clients through CopyClientMemoryForType with
 * MLG_FW_MAILBOX_MEMORY_TYPE).  A normal process that drives device
 * initialization maps it and runs a servicer loop that reads
 * <firmware root>/<name> from disk.  The protocol needs no RPC while the
 * upstream probe runs, so it works although the probe occupies the dext's
 * serial dispatch queue.
 *
 * Protocol (one outstanding request; the dext serializes requests):
 *
 *   servicer attach   servicer_pid = getpid(); servicer_generation++;
 *                     then keeps advancing servicer_heartbeat while polling.
 *   dext request      writes request_name/request_offset, then publishes
 *                     request_seq (release).  request_seq is never 0.
 *   servicer reply    reads the request after observing request_seq
 *                     (acquire), fills response_* and up to data_capacity
 *                     bytes of file data at (base + header_size), then
 *                     publishes response_seq = request_seq (release).
 *   chunking          the dext asks for offset 0 first; response_total_size
 *                     gives the file size, and further requests continue at
 *                     the next offset until the whole file is copied.
 *   missing file      response_status = MLG_FW_STATUS_NOENT.  The dext then
 *                     falls back to its optional embedded table and finally
 *                     returns -ENOENT, exactly as Linux request_firmware()
 *                     does for an absent file.
 *   servicer detach   servicer_pid = 0.  A servicer that dies without
 *                     detaching is detected by a stalled heartbeat.
 *
 * Names are the exact strings upstream passes to request_firmware(), e.g.
 * "amdgpu/gc_11_0_0_pfp.bin".  The servicer resolves them relative to its
 * firmware root (a /lib/firmware equivalent) and rejects absolute paths and
 * ".." components.
 *
 * All fields are little-endian native integers; both sides run on the same
 * host.  This header is plain C so the dext, the linuxu shim and user-space
 * clients (C, C++, Swift via a bridging header) share one definition.
 */
#ifndef LINUXU_RT_FW_MAILBOX_H
#define LINUXU_RT_FW_MAILBOX_H

#include <stdint.h>

#define MLG_FW_MAILBOX_MAGIC        0x4d4c4657u /* 'MLFW' */
#define MLG_FW_MAILBOX_VERSION      1u
/* CopyClientMemoryForType selector for the mailbox.  Values 0..5 are raw
 * BARs and values >= 0x10000 are buffer-object handles. */
#define MLG_FW_MAILBOX_MEMORY_TYPE  0x4657u
#define MLG_FW_MAILBOX_NAME_MAX     256u
#define MLG_FW_MAILBOX_HEADER_SIZE  16384u
#define MLG_FW_MAILBOX_DATA_SIZE    (1u << 20)
#define MLG_FW_MAILBOX_TOTAL_SIZE   (MLG_FW_MAILBOX_HEADER_SIZE + MLG_FW_MAILBOX_DATA_SIZE)
/* Upper bound for one firmware image; larger replies are rejected. */
#define MLG_FW_MAILBOX_MAX_FILE     (64u << 20)

/* Default firmware root used by servicers (a /lib/firmware equivalent:
 * names already carry their "amdgpu/" directory).  The installer copies a
 * linux-firmware amdgpu directory, with its WHENCE and license files, to
 * <root>/amdgpu.  MLG_FW_ROOT_ENV overrides it for development. */
#define MLG_FW_DEFAULT_ROOT "/Library/Application Support/MacLinuxGPU/firmware"
#define MLG_FW_ROOT_ENV     "MAC_LINUXGPU_FIRMWARE_ROOT"

/* response_status values (negative Linux errno numbers, which match the
 * Darwin values for these codes). */
#define MLG_FW_STATUS_OK      0
#define MLG_FW_STATUS_NOENT  (-2)
#define MLG_FW_STATUS_IO     (-5)
#define MLG_FW_STATUS_INVAL  (-22)
#define MLG_FW_STATUS_FBIG   (-27)

struct mlg_fw_mailbox {
	/* Written once by the dext when the region is created. */
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;       /* offset of the data window */
	uint32_t data_capacity;     /* bytes available in the data window */

	/* Servicer-owned. */
	uint32_t servicer_pid;      /* nonzero while a servicer is attached */
	uint32_t servicer_generation;
	uint64_t servicer_heartbeat;

	/* Dext-owned request; request_seq is published last. */
	uint32_t request_seq;
	uint32_t request_reserved;
	uint64_t request_offset;
	char     request_name[MLG_FW_MAILBOX_NAME_MAX];

	/* Servicer-owned response; response_seq is published last. */
	uint32_t response_seq;
	int32_t  response_status;
	uint64_t response_total_size;
	uint64_t response_offset;
	uint32_t response_length;
	uint32_t response_reserved;
};

#endif /* LINUXU_RT_FW_MAILBOX_H */
