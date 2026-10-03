/* linuxu shim: bug — BUG()/WARN() handlers.  The kernel's BUG() is
 * unreachable in a working system.  In the dext it goes through
 * linuxu_fatal() (record, quarantine the device, park the thread): a
 * deliberate process exit would enter PCI crash recovery.  On the host
 * shim it reports the location and aborts so a tripwire failure is loud.
 * WARN() just reports and continues (the shim keeps running). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/bug.h>
#include <rt/fatal.h>

void linuxu_bug(const char *file, int line)
{
#if LINUXU_FATAL_PARK
	linuxu_fatal("BUG", file, line);
#else
	fprintf(stderr, "linuxu BUG: %s:%d\n", file, line);
	abort();
#endif
}

void linuxu_warn(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "linuxu WARN %s:%d: ", file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}
