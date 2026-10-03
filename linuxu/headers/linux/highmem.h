/* linuxu: SHIM (third_party/linux/include/linux/highmem.h) */
#ifndef __LINUX_HIGHMEM_H
#define __LINUX_HIGHMEM_H
#include <linux/mm.h>
#endif

static inline bool mem_is_zero(const void *buf, size_t len)
{
	const unsigned long *p = buf;
	size_t n = len / sizeof(unsigned long);
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}
