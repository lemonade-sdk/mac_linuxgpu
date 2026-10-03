/* linuxu shim: fw - dext side of the on-demand firmware mailbox.
 *
 * The protocol and layout are in <rt/fw_mailbox.h>.  The dext creates the
 * shared region and attaches it here; request_firmware() then calls
 * fw_mailbox_fetch() for names that have no host-provided entry.  Without
 * an attached region or a live servicer, fetches fail immediately with
 * -ENOENT, so builds and tests without a host behave like a system whose
 * firmware directory lacks the file.
 */
#ifndef LINUXU_FW_MAILBOX_H
#define LINUXU_FW_MAILBOX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize `size` bytes at `memory` as the mailbox and publish it.  The
 * region must stay mapped until fw_mailbox_detach().  0 or -EINVAL. */
int fw_mailbox_attach(void *memory, size_t size);

/* Stop using the region; waits for an in-flight fetch to finish. */
void fw_mailbox_detach(void);

/* Nonzero if a region is attached and a servicer claims to be present. */
int fw_mailbox_servicer_present(void);

/*
 * Fetch `name` from the servicer.  On success *blob is a malloc()ed copy
 * of the whole file (free() it, or hand it to fw_table_adopt()).
 * Returns 0, -ENOENT (no servicer, file absent, servicer timed out or
 * died), -EINVAL (name not representable), -EIO/-EFBIG (malformed or
 * oversized reply) or -ENOMEM.
 */
int fw_mailbox_fetch(const char *name, uint8_t **blob, size_t *size);

/* Timeouts in milliseconds: `request_ms` bounds each chunk round trip and
 * `liveness_ms` bounds how long a stalled heartbeat is tolerated.  Zero
 * restores the defaults.  Intended for tests. */
void fw_mailbox_set_timeouts(unsigned int request_ms, unsigned int liveness_ms);

#ifdef __cplusplus
}
#endif

#endif /* LINUXU_FW_MAILBOX_H */
