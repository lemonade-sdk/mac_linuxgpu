/* Offline reproduction of PSP's first 1 MiB clear, plus memory contracts.
 * Real upstream function bodies; only register I/O and readiness are mocked. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define uuid_t darwin_uuid_t
#include <sys/mman.h>
#include <unistd.h>
#undef uuid_t
#include "amdgpu.h"
#include <rt/device_string.h>

static unsigned int waits, writes;
static int alive, wait_error, legacy;
static struct psp_context *staging_psp;
static int psp_v14_0_is_sos_alive(struct psp_context *p) { (void)p; return alive; }
static int psp_v14_0_wait_for_bootloader(struct psp_context *p)
{ (void)p; waits++; return wait_error; }

static void check_staging(void)
{
	const unsigned char *p = staging_psp->fw_pri_buf;
	for (size_t i = 0; i < PSP_1_MEG; i++)
		assert(p[i] == (i < staging_psp->kdb.size_bytes ? staging_psp->kdb.start_addr[i] : 0));
}

static void mock_write(unsigned int reg, unsigned int value)
{
	/* Publishing the mailbox must follow the complete clear and copy. */
	check_staging();
	assert(reg == (writes ? 35 : 36));
	assert(value == (writes ? PSP_BL__LOAD_KEY_DATABASE :
			(unsigned int)(staging_psp->fw_pri_mc_addr >> 20)));
	writes++;
}

static void *staging_memset(void *p, int value, size_t size)
{
	/* A simulated device range catches the old libc route without issuing
	 * DC ZVA against hardware or intentionally faulting the test process. */
	if (legacy && p == staging_psp->fw_pri_buf && !value && size == PSP_1_MEG) {
		puts("legacy PSP device-memory libc clear detected");
		exit(86);
	}
	return linuxu_device_memset(p, value, size);
}

#undef WREG32_SOC15
#define WREG32_SOC15(block, instance, reg, value) mock_write((reg), (value))
#define regMPASP_SMN_C2PMSG_35 35
#define regMPASP_SMN_C2PMSG_36 36
#undef memset
#undef memcpy
#define memset staging_memset
#define memcpy linuxu_device_memcpy
#include "upstream_psp_staging.inc"
#undef memset
#undef memcpy

static void memory_contracts(void)
{
	unsigned char source[288], expected[288], actual[288];
	for (unsigned int i = 0; i < sizeof(source); i++) source[i] = (i * 37) ^ 0xa5;
	for (size_t size = 0; size <= 256; size++) {
		for (size_t d = 0; d < 16; d++) {
			memset(expected, 0x39, sizeof(expected));
			memset(actual, 0x39, sizeof(actual));
			memset(expected + d, size & 1 ? 0 : 0x123, size);
			assert(linuxu_device_memset(actual + d, size & 1 ? 0 : 0x123, size) == actual + d);
			assert(!memcmp(actual, expected, sizeof(actual)));
			for (size_t s = 0; s < 16; s++) {
				memset(expected, 0x39, sizeof(expected));
				memset(actual, 0x39, sizeof(actual));
				memcpy(expected + d, source + s, size);
				assert(linuxu_device_memcpy(actual + d, source + s, size) == actual + d);
				assert(!memcmp(actual, expected, sizeof(actual)));
				assert(!linuxu_device_memcmp(actual, expected, sizeof(actual)));
				memcpy(expected, source, sizeof(source));
				memcpy(actual, source, sizeof(source));
				memmove(expected + d, expected + s, size);
				assert(linuxu_device_memmove(actual + d, actual + s, size) == actual + d);
				assert(!memcmp(actual, expected, sizeof(actual)));
			}
		}
	}
	assert(linuxu_device_memcmp("\xff", "\x01", 1) > 0);
	assert(linuxu_device_memcmp("\x01", "\xff", 1) < 0);
	assert(!linuxu_device_memset(NULL, 0, 0));
	assert(!linuxu_device_memcpy(NULL, NULL, 0));
	assert(!linuxu_device_memmove(NULL, NULL, 0));
	assert(!linuxu_device_memcmp(NULL, NULL, 0));
	linuxu_device_bzero(NULL, 0);

	/* Guard pages catch overreads/writes at both ends, including vector tails. */
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char *map = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(map != MAP_FAILED && !mprotect(map + page, page, PROT_READ | PROT_WRITE));
	for (size_t n = 0; n < 257; n++) {
		unsigned char *end = map + page * 2 - n;
		linuxu_device_memset(end, 0xa5, n);
		linuxu_device_memcpy(actual, end, n);
		linuxu_device_memcpy(end, source, n);
		assert(!linuxu_device_memcmp(end, source, n));
		linuxu_device_memcpy(map + page, source, n);
		linuxu_device_bzero(map + page, n);
		for (size_t i = 0; i < n; i++) assert(!map[page + i]);
	}
	assert(!munmap(map, page * 3));
}

int main(int argc, char **argv)
{
	legacy = argc == 2 && !strcmp(argv[1], "--legacy");
	struct psp_context psp = {0};
	struct amdgpu_device adev = {0};
	unsigned char firmware[4099];
	for (size_t i = 0; i < sizeof(firmware); i++) firmware[i] = (i * 17) ^ 0x53;
	unsigned char *buffer = malloc(PSP_1_MEG + 32);
	assert(buffer);
	psp.adev = &adev;
	psp.fw_pri_mc_addr = UINT64_C(0x123400000);
	psp.kdb.start_addr = firmware;
	psp.kdb.size_bytes = sizeof(firmware);
	staging_psp = &psp;
	for (size_t offset = 0; offset < 16; offset++) {
		memset(buffer, 0x9b, PSP_1_MEG + 32);
		psp.fw_pri_buf = buffer + offset;
		waits = writes = 0;
		assert(!psp_v14_0_bootloader_load_kdb(&psp));
		assert(waits == 2 && writes == 2);
		check_staging();
		for (size_t i = 0; i < offset; i++) assert(buffer[i] == 0x9b);
		for (size_t i = offset + PSP_1_MEG; i < PSP_1_MEG + 32; i++) assert(buffer[i] == 0x9b);
	}
	alive = 1; waits = writes = 0;
	assert(!psp_v14_0_bootloader_load_kdb(&psp) && !waits && !writes);
	alive = 0; wait_error = -ETIMEDOUT;
	assert(psp_v14_0_bootloader_load_kdb(&psp) == -ETIMEDOUT && waits == 1 && !writes);
	free(buffer);
	memory_contracts();
	puts("device strings: upstream PSP staging, alignment, overlaps, guards and zero lengths passed");
}
