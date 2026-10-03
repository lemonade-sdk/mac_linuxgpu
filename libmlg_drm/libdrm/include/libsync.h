/* libdrm for mac_linuxgpu: libdrm's sync_file helpers (libsync.h).
 *
 * A sync_file here is a DRM descriptor of this library standing for a
 * sync_file of the driver's Linux process (xf86drm.h, "DRM files"): poll(2)
 * and SYNC_IOC_MERGE cannot reach it, so these are functions that do the
 * same through the driver, with libdrm's names and return conventions. */
#ifndef MLG_LIBSYNC_H
#define MLG_LIBSYNC_H

#if defined(__cplusplus)
extern "C" {
#endif

/* Wait until the fence signals: 0, or -1 with errno ETIME after @timeout
 * milliseconds (negative: forever), EINVAL for a descriptor that is not a
 * sync_file of this library. */
int sync_wait(int fd, int timeout);
/* A new sync_file that signals when both have: its descriptor, or -1 with
 * errno set. */
int sync_merge(const char *name, int fd1, int fd2);
/* Accumulate @fd2 into *@fd1: dup(@fd2) when *@fd1 < 0, else replace *@fd1
 * (closing it) with the merge of both. 0, or the failed merge's result. */
int sync_accumulate(const char *name, int *fd1, int fd2);

#if defined(__cplusplus)
}
#endif

#endif
