/* libmlg_drm's GPU bring-up from a Linux-file client (mlg_init.c), against
 * a fake user client: the host window is queried, a size-aligned base
 * inside a reservation of this process's address space is chosen, then
 * InitDevice runs and the reservation is released; a driver that refuses
 * HostWindow, or answers an unusable size, still gets InitDevice. */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mlg_transport.h"
#include <rt/lx_abi.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

static struct fake_client {
	uint64_t window_bytes;		/* what the query answers */
	int window_result;		/* the query's result (-ENOTTY: refused) */
	int init_result;
	char log[16];			/* q: query, s: set, i: InitDevice */
	unsigned int n;
	uint64_t base;			/* the base set */
	int base_reserved;		/* the base's window lay in a PROT_NONE reservation */
} fake;

/* The protection of the mappings covering [@addr, @addr + @bytes) when
 * they all have the same one and leave no hole, else -1. (The kernel may
 * split a large reservation into several map entries.) */
static int protection_at(uint64_t addr, uint64_t bytes)
{
	int protection = -1;

	for (uint64_t at = addr; at < addr + bytes;) {
		mach_vm_address_t start = at;
		mach_vm_size_t size = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object = MACH_PORT_NULL;

		if (mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
				   (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
		    start > at || (protection >= 0 && info.protection != protection))
			return -1;
		protection = info.protection;
		at = start + size;
	}
	return protection;
}

static int fake_scalar(void *ctx, uint32_t selector, const uint64_t *in, uint32_t nin,
		       uint64_t *out, uint32_t nout)
{
	(void)ctx;
	CHECK(fake.n + 1 < sizeof(fake.log));
	if (selector == MLG_SELECTOR_HOST_WINDOW) {
		CHECK(nin == 1 && in && out && nout == 3);
		if (fake.window_result)
			return fake.window_result;
		if (!in[0]) {
			fake.log[fake.n++] = 'q';
			out[0] = 0;
			out[1] = fake.window_bytes;
			out[2] = 0;
			return 0;
		}
		fake.log[fake.n++] = 's';
		fake.base = in[0];
		fake.base_reserved = protection_at(in[0], fake.window_bytes) == VM_PROT_NONE;
		out[0] = in[0];
		out[1] = fake.window_bytes;
		out[2] = 1;
		return 0;
	}
	CHECK(selector == MLG_SELECTOR_INIT_DEVICE && nin == 0 && nout == 0);
	fake.log[fake.n++] = 'i';
	return fake.init_result;
}

static void reset(uint64_t bytes, int window_result, int init_result)
{
	fake = (struct fake_client){ .window_bytes = bytes, .window_result = window_result,
			       .init_result = init_result };
}

int main(void)
{
	const uint64_t gart = 512ull << 20;

	/* Query, set an aligned base inside a reservation, initialize; the
	 * reservation is gone afterwards. */
	reset(gart, 0, 0);
	CHECK(mlg_init_device(fake_scalar, NULL) == 0);
	CHECK(!strcmp(fake.log, "qsi"));
	CHECK(fake.base && fake.base % gart == 0 && fake.base_reserved);
	CHECK(protection_at(fake.base, gart) != VM_PROT_NONE);

	/* InitDevice's failure is the result; the reservation still goes. */
	reset(gart, 0, -MLG_LX_ENODEV);
	CHECK(mlg_init_device(fake_scalar, NULL) == -MLG_LX_ENODEV);
	CHECK(!strcmp(fake.log, "qsi") && fake.base_reserved);
	CHECK(protection_at(fake.base, gart) != VM_PROT_NONE);

	/* A driver that does not admit HostWindow from this client. */
	reset(gart, -MLG_LX_ENOTTY, 0);
	CHECK(mlg_init_device(fake_scalar, NULL) == 0 && !strcmp(fake.log, "i"));

	/* Sizes that cannot be a window: no base is chosen. */
	const uint64_t bad[] = { 0, 4096, 3ull << 20, 1ull << 46 };
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		reset(bad[i], 0, 0);
		CHECK(mlg_init_device(fake_scalar, NULL) == 0 && !strcmp(fake.log, "qi"));
	}
	puts("PASS libmlg_drm init: host window queried, placed size-aligned in a reservation, "
	     "InitDevice, reservation released; refused or unusable windows still initialize");
	return 0;
}
