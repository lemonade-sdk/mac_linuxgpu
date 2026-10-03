/* The production bridge runs against memory-only DriverKit substitutes. */
#include "driverkit_dma_mocks.h"
#include "../../dext/sources/iokit_bridge.mm"

extern "C" int dext_copy_bar_memory(uint8_t, uint64_t *, void **)
{
	abort(); /* CPU shmem never requests PCI memory. */
}
static IOPCIDevice cpu_test_pci;
extern "C" void shmem_test_bridge_start(void)
{
	assert(dext_dma_set_pci(&cpu_test_pci) == 0);
}
extern "C" unsigned int shmem_test_cpu_allocations(void)
{
	unsigned int count = 0;
	for (auto *entry = dext_cpu_buffers; entry; entry = entry->next) count++;
	assert(!mock_dma_prepared && !dext_dma_live_count());
	return count;
}
extern "C" void shmem_test_bridge_finish(void)
{
	assert(!shmem_test_cpu_allocations());
	assert(!dext_dma_fini());
	assert(!mock_objects && !mock_dma_prepared && mock_allocations.empty());
}
