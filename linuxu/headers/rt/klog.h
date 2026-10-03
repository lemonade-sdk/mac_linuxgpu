#ifndef LINUXU_RT_KLOG_H
#define LINUXU_RT_KLOG_H
#include <stddef.h>
#include <stdint.h>

#define LINUXU_KLOG_CAPACITY (16u * 1024u)
#define LINUXU_KLOG_MESSAGE_CAPACITY 2048u

#ifdef __cplusplus
extern "C" {
#endif
/* Read retained printk bytes without changing the log. *cursor is the next
 * byte requested and advances past the bytes returned; *end receives the
 * snapshot's exclusive end sequence (if nonnull). Neither output is a C
 * string. A stale cursor clamps to the oldest retained byte; a future cursor
 * clamps to end. The caller detects clamping by comparing the original cursor
 * with (*cursor - returned_length). With capacity zero, out may be NULL and
 * the cursor is clamped without consuming bytes. NULL cursor, or NULL out with
 * nonzero capacity, returns zero without changing outputs. No log reset occurs
 * during driver probe or cleanup. Messages exceeding the per-record capacity
 * end in an explicit truncation marker. */
size_t klog_read(uint64_t *cursor, char *out, size_t capacity, uint64_t *end);
/* Append already formatted bytes without invoking a platform log sink or the
 * printk level filter. The caller supplies a readable range; no terminator is
 * required or added. Oversized input retains its final ring-capacity bytes.
 * This is safe for platform log helpers whose sinks can reenter diagnostics. */
void klog_write(const char *text, size_t length);
#ifdef __cplusplus
}
#endif
#endif
