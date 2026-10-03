/* Real PCI admission/fault functions and DMA bridge, all RPCs replaced by
 * mocks. No DriverKit framework or installed driver is used. */
#include "driverkit_dma_mocks.h"
#include <atomic>
#include <thread>
#include <rt/dext_pci.h>
#include <rt/dext_dma.h>
#include "pci_access_gate.h"
#include "pci_reset_policy.h"
#include "../../dext/sources/iokit_bridge.mm"
/* IRQ ownership the release checks; no interrupt source exists here. */
static bool g_irq_draining;
static int g_irq_vector_count;
static void *g_irq_queue;
#include "pci_fault_production.inc"

extern "C" int dext_copy_bar_memory(uint8_t bar, uint64_t *size, void **out) {
    assert(bar == 0);
    *size = 65536;
    IOBufferMemoryDescriptor *buffer = nullptr;
    int r = IOBufferMemoryDescriptor::Create(0, *size, 16384, &buffer);
    *out = buffer;
    return r;
}

static void verify_blocked(void *allocation, void *bar) {
    const size_t calls = mock_api_calls, completions = mock_complete_calls;
    uint32_t value = 0;
    assert(g_pci_access.closed());
    assert(!dext_pci_operation() && !dext_pci_operation(false));
    assert(dext_pci_config_read32(0, &value) != 0);
    assert(dext_pci_config_write32(4, 0) != 0);
    void *cpu = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0 && !cpu);
    assert(!dext_bar0_cpu_map(0, 4096));
    assert(dext_dma_begin_reset() != 0);
    assert(dext_dma_free_coherent(allocation, 16384) != 0);
    if (bar) {
        assert(dext_bar0_cpu_unmap(bar) != 0);
        assert(dext_bar0_live_count() == 1);
    }
    assert(dext_dma_fini() != 0);
    assert(mock_api_calls == calls && mock_complete_calls == completions);
    assert(mock_dma_prepared && dext_dma_live_count());
}

int main(int argc, char **argv) {
    IOPCIDevice pci;
    g_pci = &pci; g_pci_open = true;
    assert(!dext_dma_set_pci(&pci));
    void *allocation = nullptr; uint64_t iova = 0;
    assert(!dext_dma_alloc_coherent(16384, &allocation, &iova));
    assert(allocation && iova);
    if (argc == 2 && !strcmp(argv[1], "quarantine")) {
        // A real first fault must survive the owner's later quarantine.
        dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, 0x40);
        assert(dext_pci_quarantine() == -19); // mock config space is all-ones
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_MMIO);
        assert(dext_pci_transport_fault_offset() == 0x40);
        verify_blocked(allocation, nullptr);
        // Without an earlier fault, quarantine records its own code, not
        // a CONFIG fault at the command register.
        g_transport_fault = DEXT_PCI_FAULT_NONE;
        g_transport_fault_offset = 0;
        assert(dext_pci_quarantine() == -19);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_QUARANTINE);
        assert(dext_pci_transport_fault_offset() == 0);
    } else if (argc == 2 && !strcmp(argv[1], "release")) {
        // Nothing to reopen while admission was never closed.
        assert(dext_pci_release_quarantine() == 0 && !g_pci_access.closed());
        std::atomic<bool> admitted{false}, finish{false};
        std::thread rpc([&] {
            dext_pci_operation operation;
            assert(operation);
            admitted = true;
            while (!finish) std::this_thread::yield();
        });
        while (!admitted) std::this_thread::yield();
        // An admitted RPC that has not left keeps the quarantine closed.
        assert(dext_pci_quarantine() == -16);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_QUARANTINE);
        assert(!dext_pci_quarantine_releasable());
        assert(dext_pci_release_quarantine() == -16 && g_pci_access.closed());
        finish = true; rpc.join();
        // So does a live interrupt source.
        g_irq_vector_count = 1;
        assert(!dext_pci_quarantine_releasable());
        assert(dext_pci_release_quarantine() == -16 && g_pci_access.closed());
        g_irq_vector_count = 0;
        // Only the owner's own quarantine, fully drained, may be reopened.
        assert(dext_pci_quarantine_releasable());
        assert(dext_pci_release_quarantine() == 0);
        assert(!g_pci_access.closed() && dext_pci_transport_fault() == DEXT_PCI_FAULT_NONE);
        {
            dext_pci_operation operation;
            assert(operation);
        }
        // A definite earlier fault is never reopened.
        dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, 0x80);
        assert(dext_pci_quarantine() == -19);
        assert(!dext_pci_quarantine_releasable());
        assert(dext_pci_release_quarantine() == -5 && g_pci_access.closed());
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_MMIO);
    } else if (argc == 2 && !strcmp(argv[1], "fatal")) {
        // linuxu_fatal() containment closes both seams from any thread.
        std::thread fatal([] { dext_fatal_contain("BUG", "file.c", 77); });
        fatal.join();
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_FATAL);
        assert(dext_pci_transport_fault_offset() == 77);
        verify_blocked(allocation, nullptr);
        assert(dext_pci_quarantine() == -19);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_FATAL);
    } else if (argc == 2) {
        assert(!strcmp(argv[1], "prepare"));
        mock_prepare_hook = [] {
            dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, 0x1234);
        };
        void *unpublished = nullptr;
        assert(dext_dma_alloc_coherent(16384, &unpublished, &iova) != 0 && !unpublished);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_MMIO);
        assert(dext_pci_transport_fault_offset() == 0x1234);
        assert(mock_dma_prepared == 2 && mock_complete_calls == 0);
        verify_blocked(allocation, nullptr);
    } else {
        assert(argc == 1);
        void *bar = dext_bar0_cpu_map(0, 4096);
        assert(bar);
        dext_pci_transport_record_fault(DEXT_PCI_FAULT_NONE, 0);
        dext_pci_transport_note_sentinel(DEXT_PCI_SENTINEL_MMIO, 0x20);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_NONE);
        uint32_t value = 0;
        assert(!dext_pci_config_read32(0, &value) && value == 0x744c1002);
        assert(!dext_pci_config_write32(4, 0));
        std::atomic<bool> admitted{false}, finish{false};
        std::thread rpc([&] {
            dext_pci_operation operation;
            assert(operation);
            admitted = true;
            while (!finish) std::this_thread::yield();
        });
        while (!admitted) std::this_thread::yield();
        const size_t calls = mock_api_calls;
        // The production validation error must close both seams immediately.
        assert(dext_pci_config_read32(4096, &value) != 0);
        assert(mock_api_calls == calls && !g_pci_access.drained());
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_CONFIG);
        assert(dext_pci_transport_fault_offset() == 4096);
        verify_blocked(allocation, bar);
        // Repeated faults cannot overwrite the first diagnostic or reopen.
        dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, 0x9876);
        assert(dext_pci_transport_fault() == DEXT_PCI_FAULT_CONFIG);
        assert(dext_pci_transport_fault_offset() == 4096);
        finish = true; rpc.join();
        assert(g_pci_access.drained() && !g_pci_access.enter());
    }
    puts("PCI fault containment: new RPCs rejected, DMA/BAR owners retained, no reset or release");
    // Quarantined mock mappings are intentionally retained until process exit.
}
