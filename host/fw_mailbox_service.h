/* host/fw_mailbox_service.h - user-space firmware servicer for the dext's
 * on-demand firmware mailbox (protocol: linuxu/headers/rt/fw_mailbox.h).
 *
 * Whoever drives device initialization (InitDevice) should run a servicer
 * for the duration of that call, because upstream requests firmware from
 * inside the probe:
 *
 *     struct mlg_fw_service *svc = NULL;
 *     mlg_fw_service_start_connection(connection, NULL, &svc);  // map + thread
 *     ... IOConnectCallScalarMethod(connection, 9, ...)          // InitDevice
 *     mlg_fw_service_stop(svc);                                 // join + unmap
 *
 * The servicer answers each request by reading <root>/<name>, where <root>
 * defaults to $MAC_LINUXGPU_FIRMWARE_ROOT or MLG_FW_DEFAULT_ROOT and <name>
 * is the exact request_firmware() name (e.g. "amdgpu/psp_13_0_0_sos.bin").
 * A file that does not exist is reported as missing, so the driver fails
 * the request with -ENOENT exactly as Linux does.  No device-specific list
 * exists anywhere: the servicer serves whatever upstream asks for.
 *
 * Plain C; usable from C, C++ and Swift (through a bridging header).
 */
#ifndef MLG_FW_MAILBOX_SERVICE_H
#define MLG_FW_MAILBOX_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mlg_fw_service;

/* Start servicing an already mapped mailbox region of `size` bytes.  `root`
 * may be NULL for the default firmware root.  Spawns one thread.  Returns 0
 * or a negative errno (-EINVAL for a region that is not a mailbox). */
int mlg_fw_service_start(void *mapped, size_t size, const char *root,
			 struct mlg_fw_service **out);

/* Map the dext's mailbox through an open MacLinuxGPU user-client
 * connection (an io_connect_t) and start servicing it.  Implemented in
 * fw_mailbox_iokit.c.  Returns 0 or a negative errno. */
int mlg_fw_service_start_connection(uint32_t connection, const char *root,
				    struct mlg_fw_service **out);

/* Detach, stop the thread and release the mapping if this servicer created
 * it.  NULL is ignored. */
void mlg_fw_service_stop(struct mlg_fw_service *service);

/* Counters for diagnostics: requests answered with data, and requests for
 * files that were not found. */
uint64_t mlg_fw_service_served(const struct mlg_fw_service *service);
uint64_t mlg_fw_service_missing(const struct mlg_fw_service *service);

/* Single-step API used by tests and by callers that run their own loop:
 * attach without a thread, answer at most one pending request (1 if one was
 * answered, 0 if idle), and detach. */
int mlg_fw_service_attach(void *mapped, size_t size, const char *root,
			  struct mlg_fw_service **out);
int mlg_fw_service_poll(struct mlg_fw_service *service);
void mlg_fw_service_detach(struct mlg_fw_service *service);

#ifdef __cplusplus
}
#endif

#endif /* MLG_FW_MAILBOX_SERVICE_H */
