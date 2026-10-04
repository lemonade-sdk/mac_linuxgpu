/* DriverKit's IOSysCtlByName for host builds of DriverKit-mode sources:
 * the host's sysctl answers, as the dext's does. */
#include <stddef.h>
#include <sys/sysctl.h>

int IOSysCtlByName(const char *name, void *value, size_t *length, void *replacement,
		   size_t replacement_length)
{
	return sysctlbyname(name, value, length, replacement, replacement_length);
}
