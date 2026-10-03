#ifndef LINUXU_TEST_MEMORY_SYSCTL_H
#define LINUXU_TEST_MEMORY_SYSCTL_H

/* Deterministic host-memory boundary for offline upstream integration tests.
 * Production uses the real platform adapter; these fixtures never query it. */
#ifdef __APPLE__
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>

int sysctlbyname(const char *name, void *out, size_t *length,
		 void *replacement, size_t replacement_length)
{
	assert(name && out && length && !replacement && !replacement_length);
	if (!strcmp(name, "hw.memsize")) {
		const uint64_t bytes = 64ull << 30;
		assert(*length == sizeof(bytes));
		memcpy(out, &bytes, sizeof(bytes));
	} else {
		uint32_t value;
		if (!strcmp(name, "hw.pagesize")) value = 16384;
		else {
			assert(!strcmp(name, "vm.page_free_count"));
			value = 4096;
		}
		assert(*length == sizeof(value));
		memcpy(out, &value, sizeof(value));
	}
	return 0;
}
#endif
#endif
