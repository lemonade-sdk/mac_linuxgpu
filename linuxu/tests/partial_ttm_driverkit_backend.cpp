#include "driverkit_dma_mocks.h"
#include "../../dext/sources/iokit_bridge.mm"
extern "C" int dext_copy_bar_memory(uint8_t, uint64_t *, void **) { abort(); }
static IOPCIDevice test_pci;
extern "C" void partial_ttm_bridge_start(void)
{
	assert(!dext_dma_set_pci(&test_pci));
	assert(!dext_dma_begin_probe(1536ull << 20));
}
extern "C" void partial_ttm_bridge_check(unsigned live, unsigned retired)
{
	unsigned active = 0, held = 0;
	for (auto &entry : dext_dma_table) {
		if (entry.in_use && !entry.retired) active++;
		if (entry.in_use && entry.retired) held++;
	}
	assert(active == live && held == retired && !mock_complete_calls);
}
extern "C" void partial_ttm_bridge_finish(void)
{
	assert(!dext_dma_begin_shutdown(1536ull << 20));
	assert(!dext_dma_begin_shutdown_reset());
	assert(!dext_dma_end_shutdown_reset(1));
	assert(!dext_dma_live_count() && !dext_dma_fini());
	assert(!mock_objects && !mock_dma_prepared && mock_allocations.empty());
	mock_complete_calls = 0;
}
