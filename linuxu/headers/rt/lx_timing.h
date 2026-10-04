/* Where the time of a Linux-file request goes, hop by hop.
 *
 * The dext's user client and the Linux-file runtime (lx_files.c) time each
 * request's hops with the uptime clock and add them here, per DRM/KFD
 * request number (the command's low byte). The device's sysfs file
 * mlg_lx_timing shows, per request number, the count and the mean of each
 * hop in nanoseconds; the totals only grow, so a reader takes the
 * difference of two reads (scripts/read-sysfs.py mlg_lx_timing).
 *
 * The hops of one LX_IOCTL, in order:
 *   admit    user client: entry to the Linux-file state checked
 *   args     user client: request frame and reply buffer set up
 *   frame    frame checked and copied, call admitted (rt_lx_ioctl)
 *   pages    the argument pages mapped into the Linux process
 *   copyin   IN segments copied to the pages
 *   enter    the calling thread becomes a thread of the Linux process
 *   ioctl    the driver's unlocked_ioctl
 *   leave    the thread leaves the Linux process
 *   copyout  OUT segments copied to the reply
 *   release  the argument pages unmapped
 *   finish   call ended, frame copy freed
 *   reply    user client: reply handed to IOKit, return
 *   total    user client: entry to return
 * An LX_IOCTL_ASYNC request (a wait) has no args or reply hops; its
 * "spawn" hop is the time from the request to its worker starting. */
#ifndef LINUXU_RT_LX_TIMING_H
#define LINUXU_RT_LX_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rt_lx_hop {
	RT_LX_HOP_ADMIT,
	RT_LX_HOP_ARGS,
	RT_LX_HOP_FRAME,
	RT_LX_HOP_PAGES,
	RT_LX_HOP_COPYIN,
	RT_LX_HOP_ENTER,
	RT_LX_HOP_IOCTL,
	RT_LX_HOP_LEAVE,
	RT_LX_HOP_COPYOUT,
	RT_LX_HOP_RELEASE,
	RT_LX_HOP_FINISH,
	RT_LX_HOP_REPLY,
	RT_LX_HOP_TOTAL,
	RT_LX_HOP_SPAWN,
	RT_LX_HOP_COUNT
};

/* The uptime clock in nanoseconds. */
uint64_t rt_lx_time_ns(void);
/* Add @ns to hop @hop of request @cmd (counted once per request, on TOTAL
 * for synchronous requests and on IOCTL for asynchronous ones). */
void rt_lx_timing_add(uint32_t cmd, enum rt_lx_hop hop, uint64_t ns);
/* The sysfs report: writes at most @size bytes to @buf, returns the length. */
long rt_lx_timing_show(char *buf, unsigned long size);
struct device;
/* Adds the device's mlg_lx_timing file if it is not there yet. */
void rt_lx_timing_register(struct device *dev);

#ifdef __cplusplus
}
#endif
#endif
