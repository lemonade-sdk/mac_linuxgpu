/* linuxu shim: string — kernel string helpers NOT covered by the libc
 * passthrough in linuxu/headers/linux/string.h
 * (REAL).  Kernel arg order preserved where it
 * differs from libc (e.g. strnchr, strim semantics). */
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>

#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/errno.h>
#include <linux/types.h>

/* kstrtox runtime (kstrtoll/kstrtoull/_kstrtol/_kstrtoul) and vscnprintf
 * live in shims/printk.c — the single definition point in linuxu/src
 * (the old local copies here caused duplicate-symbol link failures). */

/* sized_strscpy (runtime behind linux/string.h's strscpy macro):
 * bounded copy, always NUL-terminates; returns -E2BIG if truncated,
 * else the length copied (kernel semantics). */
ssize_t sized_strscpy(char *dst, const char *src, size_t size)
{
	if (!size) return -E2BIG;
	for (size_t i = 0; i < size; i++) {
		char value = src[i];
		dst[i] = value;
		if (!value) return (ssize_t)i;
	}
	dst[size - 1] = '\0';
	return -E2BIG;
}

ssize_t sized_strscpy_pad(char *dst, const char *src, size_t size)
{
	ssize_t copied = sized_strscpy(dst, src, size);
	if (copied >= 0)
		memset(dst + copied, 0, size - (size_t)copied);
	return copied;
}

/* strnchr / simple_strtol / strncasecmp / strcasecmp / strim are
 * static inlines in the enriched linux/string.h — the old local
 * definitions here conflicted with them, so they were dropped.
 */

/* ---- format helpers (printk/vsprintf family; thin over libc) ---- */
int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0) return n;
	return !size ? 0 : ((size_t)n < size ? n : (int)(size - 1));
}

/* vsscanf: not in the kernel API surface the driver uses (libc's
 * vsscanf conflicts with a local definition of the same name); the
 * old local definition was dropped. kstrdup/kstrdup_const live in
 * kmem/slab.c (the single definition point in linuxu/src). */
