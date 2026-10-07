/* A client's revocation of its BAR mapping (hsa/src/bar_mapping_retire.h),
 * on the host: the "device" is a region of memory, the client's mapping a
 * second mapping of the same pages (mach_vm_remap, shared, as IOKit maps a
 * BAR into a client), and writer threads store into the client's mapping
 * without any gate, as scattered stores do (a memcpy into a kernarg ring,
 * a register store and its read back).
 *
 * What it proves: once the retire returns, no store of any thread reaches
 * the device's pages, though the writers keep storing; none of them
 * faults; loads of the retired range read zero (Linux's dummy page); and
 * the retire is one step (no moment where the range is unmapped). */
#include "bar_mapping_retire.h"
#include <assert.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WRITERS 6u
#define PAGES 8u

static size_t bytes;
static mach_vm_address_t device, client;
static _Atomic int stop;
static _Atomic uint64_t stores[WRITERS];

static void *writer(void *arg)
{
	const unsigned n = (unsigned)(uintptr_t)arg;
	volatile uint8_t *base = (volatile uint8_t *)(uintptr_t)client;
	uint8_t args[192];
	uint64_t v = 0;

	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		/* A submission's kernargs, somewhere in the ring... */
		const size_t at = ((v * 7u + n * 977u) % (bytes / 64u - 3u)) * 64u;

		memset(args, (int)(0x10u + n), sizeof(args));
		memcpy((void *)(uintptr_t)(base + at), args, sizeof(args));
		/* ...a register store and its read back. */
		*(volatile uint32_t *)(uintptr_t)(base + bytes - 4u * (n + 1u)) = (uint32_t)v | 1u;
		(void)*(volatile uint32_t *)(uintptr_t)(base + bytes - 4u * (n + 1u));
		atomic_fetch_add_explicit(&stores[n], 1, memory_order_relaxed);
		++v;
	}
	return NULL;
}

static uint64_t total(void)
{
	uint64_t t = 0;

	for (unsigned i = 0; i < WRITERS; ++i)
		t += atomic_load(&stores[i]);
	return t;
}

int main(void)
{
	pthread_t threads[WRITERS];
	vm_prot_t cur = 0, max = 0;
	uint8_t *snapshot;

	bytes = (size_t)PAGES * (size_t)vm_page_size;
	assert(mach_vm_allocate(mach_task_self(), &device, bytes, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
	memset((void *)(uintptr_t)device, 0, bytes);
	assert(mach_vm_remap(mach_task_self(), &client, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), device,
			     FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS);
	/* The two are one memory, as a BAR and its client mapping are. */
	((volatile uint8_t *)(uintptr_t)client)[0] = 0x5a;
	assert(((volatile uint8_t *)(uintptr_t)device)[0] == 0x5a);

	for (unsigned i = 0; i < WRITERS; ++i)
		assert(!pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)i));
	while (total() < 100000u)
		usleep(1000);

	/* The device goes: retire the client's mapping under the writers. */
	const uint64_t before = total();
	assert(mac_hsa_bar_mapping_retire((void *)(uintptr_t)client, bytes) == 0);
	snapshot = malloc(bytes);
	assert(snapshot);
	memcpy(snapshot, (const void *)(uintptr_t)device, bytes);

	/* The writers keep storing (to the anonymous page now)... */
	const uint64_t at_retire = total();
	usleep(200000);
	const uint64_t after = total();
	assert(after > at_retire + 10000u);
	/* ...and not one store reached the device's pages. */
	assert(memcmp(snapshot, (const void *)(uintptr_t)device, bytes) == 0);

	atomic_store(&stop, 1);
	for (unsigned i = 0; i < WRITERS; ++i)
		pthread_join(threads[i], NULL);
	assert(memcmp(snapshot, (const void *)(uintptr_t)device, bytes) == 0);

	/* A fresh retire reads zero where nothing has stored since. */
	assert(mac_hsa_bar_mapping_retire((void *)(uintptr_t)client, bytes) == 0);
	for (size_t i = 0; i < bytes; ++i)
		assert(((volatile uint8_t *)(uintptr_t)client)[i] == 0);
	/* Retiring a range that is not mapped at a fixed address it can take
	 * fails without touching anything (a misaligned address). */
	assert(mac_hsa_bar_mapping_retire((void *)(uintptr_t)(client + 1), bytes) != 0);

	printf("test_bar_mapping_retire: %llu stores before the retire, %llu after it, none reached "
	       "the device's pages\n",
	       (unsigned long long)before, (unsigned long long)(after - at_retire));
	free(snapshot);
	return 0;
}
