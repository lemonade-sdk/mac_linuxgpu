/* Exercises the real LINUXU_DEXT bridge with mocked DriverKit RPC boundaries. */
#include "driverkit_dma_mocks.h"
#include "../../dext/sources/iokit_bridge.mm"
#include <rt/dext_dma.h>

static uint64_t log_tail() {
    uint64_t cursor = UINT64_MAX;
    assert(klog_read(&cursor, nullptr, 0, nullptr) == 0);
    return cursor;
}
static void expect_dma_log(uint64_t cursor, const char *message) {
    char text[LINUXU_KLOG_CAPACITY + 1];
    const size_t length = klog_read(&cursor, text, sizeof(text) - 1, nullptr);
    text[length] = 0;
    assert(strstr(text, message));
}

extern "C" int dext_copy_bar_memory(uint8_t bar, uint64_t *size, void **out) {
    assert(bar == 0); *size = 65536;
    IOBufferMemoryDescriptor *buffer = nullptr;
    int r = IOBufferMemoryDescriptor::Create(0, *size, 16384, &buffer);
    *out = buffer; return r;
}

static IOPCIDevice pci;
static void start() { assert(dext_dma_set_pci(&pci) == 0); }
static void clean() {
    assert(dext_dma_live_count() == 0);
    assert(dext_dma_fini() == 0);
    assert(mock_objects == 0 && mock_dma_prepared == 0);
    assert(mock_allocations.empty());
}
static void *allocate() {
    void *cpu = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) == 0 && cpu && iova);
    return cpu;
}
static void reset_admission() {
    start();
    assert(dext_dma_begin_reset() == 0);
    assert(dext_dma_begin_reset() != 0);
    void *cpu = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0 && !cpu);
    assert(!dext_bar0_cpu_map(0, 4096));
    assert(dext_dma_set_pci(&pci) != 0);
    dext_dma_end_reset();
    cpu = allocate();
    assert(dext_dma_begin_reset() != 0);
    assert(dext_dma_free_coherent(cpu, 16384) == 0);
    mock_prepare_hook = [] { assert(dext_dma_begin_reset() != 0); };
    cpu = allocate();
    assert(dext_dma_free_coherent(cpu, 16384) == 0);
    clean();
}
static void shutdown_retention() {
    start();
    void *first = allocate(), *second = allocate();
    const size_t completions = mock_complete_calls;
    assert(dext_dma_begin_shutdown(16384) != 0); /* below actual pinned bytes */
    assert(dext_dma_begin_shutdown(65536) == 0);
    assert(dext_dma_begin_shutdown(65536) != 0);
    assert(dext_dma_begin_reset() != 0);
    assert(dext_dma_free_coherent(first, 16384) == 0);
    assert(mock_complete_calls == completions && mock_dma_prepared == 2);
    assert(!dext_dma_copy_descriptor(first));
    assert(dext_dma_free_coherent(first, 16384) != 0);
    auto cursor = log_tail();
    const size_t calls_before_rejection = mock_api_calls;
    assert(dext_dma_begin_shutdown_reset() != 0); /* second is still owned */
    assert(mock_api_calls == calls_before_rejection); /* cached diagnostics only */
    expect_dma_log(cursor, "DMA shutdown reset has live owner:");
    expect_dma_log(cursor, "live=1 retired=1 bytes=32768/65536");
    const void *page = second;
    void *alias = dext_dma_vmap_pages(&page, 1);
    assert(alias);
    assert(dext_dma_free_coherent(second, 16384) == 0);
    cursor = log_tail();
    assert(dext_dma_begin_shutdown_reset() != 0); /* CPU alias still in use */
    expect_dma_log(cursor, "DMA shutdown reset rejected:");
    expect_dma_log(cursor, "cpu=0 vmaps=1 live=0 retired=2 bytes=32768/65536");
    dext_dma_vunmap_pages(alias);
    mock_fail_api = mock_api_calls + 1;
    void *failed = nullptr; uint64_t failed_iova = 0;
    assert(dext_dma_alloc_coherent(16384, &failed, &failed_iova) != 0);
    mock_fail_api = 0; /* reservation must refund before PrepareForDMA */
    mock_prepare_hook = [] { assert(dext_dma_begin_shutdown_reset() != 0); };
    void *third = allocate();
    assert(dext_dma_free_coherent(third, 16384) == 0);
    /* Failed unpublished mappings are retained by the same shutdown hold. */
    mock_short_segment = true;
    void *unused = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &unused, &iova) != 0);
    mock_short_segment = false;
    assert(mock_dma_prepared == 4 && mock_complete_calls == completions);
    /* Linux returned all four allocations, but their deferred DART mappings
     * remain charged. A fifth allocation must not reach PrepareForDMA. */
    const size_t calls = mock_api_calls;
    assert(dext_dma_alloc_coherent(16384, &unused, &iova) != 0);
    assert(mock_api_calls == calls && mock_dma_prepared == 4);
    assert(dext_dma_begin_shutdown_reset() == 0);
    assert(dext_dma_alloc_coherent(16384, &unused, &iova) != 0 && !unused);
    assert(dext_dma_set_pci(&pci) != 0);
    assert(dext_dma_end_shutdown_reset(1) == 0);
    assert(mock_complete_calls == completions + 4);
    clean();

    start();
    mock_prepare_hook = [] { assert(dext_dma_begin_shutdown(65536) != 0); };
    third = allocate();
    mock_complete_hook = [] { assert(dext_dma_begin_shutdown(65536) != 0); };
    assert(dext_dma_free_coherent(third, 16384) == 0);
    clean();

    start(); first = allocate();
    assert(dext_dma_begin_shutdown(32768) == 0);
    assert(dext_dma_free_coherent(first, 16384) == 0);
    mock_prepare_hook = [] {
        void *nested = nullptr; uint64_t address = 0;
        assert(dext_dma_alloc_coherent(16384, &nested, &address) != 0);
    };
    second = allocate(); /* reservation also accounts for an in-flight map */
    assert(dext_dma_free_coherent(second, 16384) == 0);
    assert(dext_dma_begin_shutdown_reset() == 0);
    assert(dext_dma_end_shutdown_reset(1) == 0);
    clean();
}
static void shutdown_failure(bool completion_failure) {
    start(); void *cpu = allocate();
    assert(dext_dma_begin_shutdown(65536) == 0);
    assert(dext_dma_free_coherent(cpu, 16384) == 0);
    assert(dext_dma_begin_shutdown_reset() == 0);
    mock_complete_failure = completion_failure;
    assert(dext_dma_end_shutdown_reset(completion_failure ? 1 : 0) != 0);
    assert(mock_complete_calls == (completion_failure ? 1u : 0u));
    assert(mock_dma_prepared == 1 && mock_objects == 2);
    assert(dext_dma_fini() != 0 && dext_dma_set_pci(&pci) != 0);
    assert(dext_dma_free_coherent(cpu, 16384) != 0);
    /* Reset/CompleteDMA failure deliberately keeps both original owners. */
}
static void probe_lifetime() {
    start();
    assert(dext_dma_begin_probe(65536) == 0);
    void *live = allocate(), *retired = allocate(), *added = nullptr;
    assert(dext_dma_free_coherent(retired, 16384) == 0);
    const size_t before = mock_complete_calls;
    mock_complete_hook = [&] {
        /* A worker may retire an earlier table slot or allocate while the
         * probe commits. Neither operation may spuriously fail as a reset. */
        assert(dext_dma_free_coherent(live, 16384) == 0);
        added = allocate();
        void *temporary = allocate();
        assert(dext_dma_free_coherent(temporary, 16384) == 0);
    };
    assert(dext_dma_commit_probe() == 0);
    assert(added && mock_dma_prepared == 1 && mock_complete_calls == before + 3);
    assert(dext_dma_live_count() == 1);
    assert(dext_dma_free_coherent(added, 16384) == 0);
    clean();

    start(); assert(dext_dma_begin_probe(65536) == 0);
    retired = allocate();
    const size_t old = mock_complete_calls;
    assert(dext_dma_free_coherent(retired, 16384) == 0);
    /* This models cleanup inside a failed probe before its caller sees the
     * error. Only successful shutdown reset may release the retired DMA. */
    assert(mock_dma_prepared == 1 && mock_complete_calls == old);
    assert(dext_dma_begin_shutdown(65536) == 0);
    assert(dext_dma_commit_probe() != 0);
    assert(dext_dma_begin_shutdown_reset() == 0);
    assert(dext_dma_end_shutdown_reset(1) == 0);
    clean();
}
static void probe_commit_failure() {
    start(); assert(dext_dma_begin_probe(65536) == 0);
    void *cpu = allocate();
    assert(dext_dma_free_coherent(cpu, 16384) == 0);
    mock_complete_failure = true;
    assert(dext_dma_commit_probe() != 0);
    assert(mock_dma_prepared == 1 && mock_objects == 2);
    assert(dext_dma_fini() != 0 && dext_dma_set_pci(&pci) != 0);
    assert(dext_dma_begin_shutdown_reset() != 0);
}
static void quarantine_inflight(bool completing) {
    start();
    void *cpu = allocate();
    if (completing) {
        mock_complete_hook = [] { dext_dma_quarantine(); };
        assert(dext_dma_free_coherent(cpu, 16384) != 0);
        /* The already-issued RPC cannot be cancelled, but backing must
         * remain owned when it returns into a quarantined session. */
        assert(mock_complete_calls == 1 && mock_dma_prepared == 0);
        assert(mock_objects == 2);
        memset(cpu, 0xa5, 16384);
    } else {
        void *pages = dext_cpu_alloc_pages(16384); assert(pages);
        const void *page = pages;
        void *alias = dext_dma_vmap_pages(&page, 1); assert(alias);
        void *bar = dext_bar0_cpu_map(0, 4096); assert(bar);
        mock_prepare_hook = [] {
            assert(dext_dma_begin_shutdown(65536) != 0);
            dext_dma_quarantine();
        };
        void *pending = nullptr; uint64_t address = 0;
        assert(dext_dma_alloc_coherent(16384, &pending, &address) != 0);
        assert(!pending && mock_dma_prepared == 2 && mock_complete_calls == 0);
        const size_t objects = mock_objects;
        assert(dext_dma_free_coherent(cpu, 16384) != 0);
        assert(dext_cpu_free_pages(pages, 16384) != 0);
        dext_dma_vunmap_pages(alias);
        assert(dext_bar0_cpu_unmap(bar) != 1);
        assert(mock_objects == objects && mock_complete_calls == 0);
    }
    void *unused = nullptr; uint64_t address = 0;
    assert(dext_dma_alloc_coherent(16384, &unused, &address) != 0);
    assert(!dext_cpu_alloc_pages(16384));
    assert(!dext_dma_copy_descriptor(cpu));
    assert(dext_dma_fini() != 0 && dext_dma_set_pci(&pci) != 0);
    assert(dext_dma_begin_reset() != 0 && dext_dma_begin_shutdown_reset() != 0);
}
static void normal_and_rpc_failures() {
    for (size_t step = 1; step <= 5; ++step) {
        start(); mock_api_calls = 0; mock_fail_api = step;
        void *cpu = reinterpret_cast<void *>(1); uint64_t iova = 1;
        assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0);
        assert(!cpu && !iova); mock_fail_api = 0; clean();
    }
    start(); void *cpu = allocate();
    auto *descriptor = static_cast<IOMemoryDescriptor *>(dext_dma_copy_descriptor(cpu));
    assert(descriptor); descriptor->release();
    mock_complete_hook = [cpu] {
        assert(dext_dma_live_count() == 1);
        assert(dext_dma_fini() != 0);
        assert(dext_dma_free_coherent(cpu, 16384) != 0);
        assert(dext_dma_copy_descriptor(cpu) == nullptr);
    };
    assert(dext_dma_free_coherent(cpu, 16384) == 0); clean();
    start(); mock_short_segment = true;
    uint64_t iova;
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0);
    mock_short_segment = false; clean();
}
static void stopping_during_allocation() {
    start(); mock_prepare_hook = [] { assert(dext_dma_fini() != 0); };
    void *cpu = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0 && !cpu && !iova);
    clean();
    start(); mock_map_hook = [] { assert(dext_dma_fini() != 0); };
    assert(dext_bar0_cpu_map(0, 4096) == nullptr); clean();
    start(); void *bar = dext_bar0_cpu_map(4096, 4096); assert(bar);
    assert(dext_bar0_cpu_contains(bar, 4096));
    assert(dext_dma_fini() != 0);
    assert(!dext_bar0_cpu_map(0, 4096));
    assert(dext_bar0_cpu_unmap(bar) == 1);
    assert(!dext_bar0_cpu_contains(bar, 1)); clean();
}
static void vmap_unwind() {
    start();
    std::vector<const void *> pages;
    for (unsigned i = 0; i < 65; ++i) pages.push_back(allocate());
    const size_t baseline = mock_objects;
    mock_api_calls = 0;
    void *alias = dext_dma_vmap_pages(pages.data(), pages.size());
    assert(alias); size_t operations = mock_api_calls;
    dext_dma_vunmap_pages(alias);
    assert(mock_objects == baseline && mock_allocations.empty());
    /* Every subdescriptor, concatenation level, and final map can fail. */
    for (size_t step = 1; step <= operations; ++step) {
        mock_api_calls = 0; mock_fail_api = step;
        assert(!dext_dma_vmap_pages(pages.data(), pages.size()));
        assert(mock_objects == baseline && mock_allocations.empty());
    }
    mock_fail_api = 0;
    /* Exact IOFree sizes are checked for initial and folded descriptor arrays. */
    for (size_t step = 1; step <= 4; ++step) {
        mock_fail_allocation = step;
        assert(!dext_dma_vmap_pages(pages.data(), pages.size()));
        assert(mock_objects == baseline && mock_allocations.empty());
    }
    mock_fail_allocation = 0;
    assert(!dext_dma_vmap_pages(pages.data(), SIZE_MAX / 16384 + 1));
    alias = dext_dma_vmap_pages(pages.data(), pages.size()); assert(alias);
    for (auto p : pages) assert(dext_dma_free_coherent(const_cast<void *>(p), 16384) == 0);
    assert(dext_dma_live_count() == 0);
    assert(dext_dma_fini() != 0); /* aliases still retain coherent backing */
    dext_dma_vunmap_pages(alias); clean();

    start(); void *cpu = allocate(); const void *page = cpu;
    mock_map_hook = [] { assert(dext_dma_fini() != 0); };
    assert(!dext_dma_vmap_pages(&page, 1));
    assert(dext_dma_free_coherent(cpu, 16384) == 0); clean();
}
static void cpu_pages_and_aliases() {
	for (size_t step = 1; step <= 3; step++) {
		start(); mock_api_calls = 0; mock_fail_api = step;
		assert(!dext_cpu_alloc_pages(16384));
		mock_fail_api = 0; clean();
	}
	start(); mock_fail_allocation = 1;
	assert(!dext_cpu_alloc_pages(16384));
	mock_fail_allocation = 0;
	assert(!dext_cpu_alloc_pages(1));
	clean();
	start(); mock_address_hook = [] { assert(dext_dma_fini() != 0); };
	assert(!dext_cpu_alloc_pages(16384));
	clean();
	start();
	auto *first = static_cast<unsigned char *>(dext_cpu_alloc_pages(32768));
	auto *second = static_cast<unsigned char *>(dext_cpu_alloc_pages(16384));
	assert(first && second && mock_dma_prepared == 0 && dext_dma_live_count() == 0);
	assert(dext_cpu_free_pages(first, 1) != 0);
	assert(dext_cpu_free_pages(first + 16384, 16384) != 0);
	memset(first, 0x17, 16384); memset(first + 16384, 0x39, 16384); memset(second, 0x5b, 16384);
	const void *pages[] = {second, first + 16384, first};
	const size_t objects = mock_objects, allocations = mock_allocations.size();
	mock_api_calls = 0;
	auto *alias = static_cast<unsigned char *>(dext_dma_vmap_pages(pages, 3));
	assert(alias);
	size_t calls = mock_api_calls;
	assert(alias[0] == 0x5b && alias[16384] == 0x39 && alias[32768] == 0x17);
	alias[11] = 0xa4; assert(second[11] == 0xa4);
	first[16384 + 13] = 0xe6; assert(alias[16384 + 13] == 0xe6);
	dext_dma_vunmap_pages(alias);
	for (size_t step = 1; step <= calls; step++) {
		mock_api_calls = 0; mock_fail_api = step;
		assert(!dext_dma_vmap_pages(pages, 3));
		assert(mock_objects == objects && mock_allocations.size() == allocations && !mock_dma_prepared);
	}
	mock_fail_api = 0;
	alias = static_cast<unsigned char *>(dext_dma_vmap_pages(pages, 3));
	assert(alias && !mock_dma_prepared);
	assert(!dext_cpu_free_pages(first, 32768) && !dext_cpu_free_pages(second, 16384));
	assert(dext_cpu_free_pages(second, 16384) != 0);
	assert(alias[11] == 0xa4 && alias[16384 + 13] == 0xe6);
	assert(dext_dma_fini() != 0);
	dext_dma_vunmap_pages(alias);
	clean();
}
/* A client mapping of a BO whose pages are several allocations (a KFD
 * process's GTT BO): one descriptor over the runs, in order. */
static void ranges_descriptor() {
	start();
	auto *a = static_cast<unsigned char *>(allocate());
	auto *b = static_cast<unsigned char *>(allocate());
	auto *pages = static_cast<unsigned char *>(dext_cpu_alloc_pages(32768));
	assert(pages);
	memset(a, 0x11, 16384); memset(b, 0x22, 16384);
	memset(pages, 0x33, 16384); memset(pages + 16384, 0x44, 16384);
	const uint64_t addresses[] = {(uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)(pages + 16384),
				      (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)pages};
	const uint64_t lengths[] = {16384, 16384, 16384, 16384};
	const size_t objects = mock_objects, allocations = mock_allocations.size();
	mock_api_calls = 0;
	auto *descriptor = static_cast<IOMemoryDescriptor *>(
		dext_dma_copy_ranges_descriptor(addresses, lengths, 4));
	const size_t calls = mock_api_calls;
	assert(descriptor && descriptor->length == 65536);
	IOMemoryMap *map = nullptr;
	assert(descriptor->CreateMapping(0, 0, 0, 0, 0, &map) == 0 && map);
	auto *bytes = reinterpret_cast<unsigned char *>(map->GetAddress());
	assert(bytes[0] == 0x22 && bytes[16384] == 0x44 && bytes[32768] == 0x11 && bytes[49152] == 0x33);
	bytes[5] = 0x99; assert(b[5] == 0x99);	/* the client writes the BO's pages */
	map->release(); descriptor->release();
	assert(mock_objects == objects);
	/* A run of two pages inside one allocation. */
	const uint64_t run[] = {(uint64_t)(uintptr_t)pages}, run_length[] = {32768};
	descriptor = static_cast<IOMemoryDescriptor *>(dext_dma_copy_ranges_descriptor(run, run_length, 1));
	assert(descriptor && descriptor->length == 32768);
	descriptor->release();
	/* A run that leaves its allocation, an empty run, or nothing at all. */
	const uint64_t overrun[] = {(uint64_t)(uintptr_t)a}, overrun_length[] = {32768}, empty[] = {0};
	assert(!dext_dma_copy_ranges_descriptor(overrun, overrun_length, 1));
	assert(!dext_dma_copy_ranges_descriptor(overrun, empty, 1));
	assert(!dext_dma_copy_ranges_descriptor(overrun, overrun_length, 0));
	assert(mock_objects == objects && mock_allocations.size() == allocations);
	/* Every subdescriptor and concatenation can fail without leaking. */
	for (size_t step = 1; step <= calls; ++step) {
		mock_api_calls = 0; mock_fail_api = step;
		assert(!dext_dma_copy_ranges_descriptor(addresses, lengths, 4));
		assert(mock_objects == objects && mock_allocations.size() == allocations);
	}
	mock_fail_api = 0;
	assert(dext_dma_free_coherent(a, 16384) == 0 && dext_dma_free_coherent(b, 16384) == 0);
	assert(!dext_cpu_free_pages(pages, 32768));
	clean();
}
/* Apple DART windows on later SoCs start above 1 TiB; this one sits at
 * 1 TiB + 2 GiB, reachable with 41 address bits but not with 40. */
static constexpr uint64_t kHighDartIova = (1ull << 40) + (2ull << 30);
static int map_once(uint64_t at) {
    void *cpu = nullptr; uint64_t address = 0;
    mock_dart_iova = at;
    const int r = dext_dma_alloc_coherent(16384, &cpu, &address);
    if (r == 0) {
        assert(cpu && address == at);
        assert(dext_dma_free_coherent(cpu, 16384) == 0);
    } else assert(!cpu && !address);
    return r;
}
/* The device's DMA mask width (32, 40, 44, 64 bits) reaches every
 * IODMACommand as maxAddressBits, and a mapping the DART places beyond it
 * is completed and never published. */
static void address_widths() {
    start();
    assert(dext_dma_address_bits() == 64); /* platform placement until a mask is set */
    assert(dext_dma_set_address_bits(31) != 0 && dext_dma_set_address_bits(65) != 0);
    assert(dext_dma_address_bits() == 64);
    const auto cursor = log_tail();
    for (unsigned bits : {32u, 40u, 44u, 64u}) {
        assert(dext_dma_set_address_bits(bits) == 0 && dext_dma_address_bits() == bits);
        assert(map_once(0x100000) == 0 && mock_last_address_bits == bits);
        if (bits < 64) {
            assert(map_once((1ull << bits) - 16384) == 0); /* last page fits */
            assert(map_once((1ull << bits) - 4096) != 0);  /* straddles 2^bits */
            assert(map_once(1ull << bits) != 0);
        } else {
            assert(map_once(UINT64_MAX - 16383) == 0);
            assert(map_once(UINT64_MAX - 4095) != 0); /* would wrap */
        }
        assert((map_once(kHighDartIova) == 0) == (bits > 40));
        assert(dext_dma_live_count() == 0 && mock_dma_prepared == 0);
    }
    expect_dma_log(cursor, "DMA mapping refused: the DART placed it at 0x10080000000, above the device's 40-bit DMA mask");
    assert(dext_dma_set_address_bits(64) == 0);
    mock_dart_iova = 0x100000;
    clean();
}
/* The platform probe asks the mapper itself and caches monotonic answers. */
static void platform_probe() {
    assert(dext_dma_platform_supports_bits(44) == -1); /* no provider: unknown */
    start();
    assert(dext_dma_platform_supports_bits(31) == 0);
    mock_dart_iova = kHighDartIova; /* a mapper that ignores maxAddressBits */
    uint64_t commands = mock_dma_commands;
    assert(dext_dma_platform_supports_bits(40) == 0 && mock_last_address_bits == 40);
    assert(mock_dma_commands == ++commands);
    assert(dext_dma_platform_supports_bits(32) == 0 && mock_dma_commands == commands);
    assert(dext_dma_platform_supports_bits(44) == 1 && mock_dma_commands == ++commands);
    assert(dext_dma_platform_supports_bits(64) == 1 && dext_dma_platform_supports_bits(48) == 1);
    assert(mock_dma_commands == commands);
    assert(dext_dma_platform_supports_bits(41) == 1 && mock_dma_commands == ++commands);
    assert(dext_dma_live_count() == 0 && mock_dma_prepared == 0);
    clean();
    /* A new provider is asked again: a DART below 4 GiB serves 32 bits. */
    start(); mock_dart_iova = 0x80000000;
    commands = mock_dma_commands;
    assert(dext_dma_platform_supports_bits(32) == 1 && mock_dma_commands == ++commands);
    assert(dext_dma_platform_supports_bits(40) == 1 && mock_dma_commands == commands);
    clean();
    /* A mapper that refuses narrow commands: the refusal is attributed to
     * the width only when an unconstrained mapping succeeds. */
    start(); mock_dart_iova = 0x100000; mock_dart_refuse_below_bits = 44;
    assert(dext_dma_platform_supports_bits(40) == 0);
    assert(dext_dma_platform_supports_bits(44) == 1);
    mock_dart_refuse_below_bits = 65;
    assert(dext_dma_platform_supports_bits(42) == -1); /* nothing maps: unknown */
    mock_dart_refuse_below_bits = 0;
    assert(dext_dma_platform_supports_bits(42) == 1);
    assert(dext_dma_live_count() == 0 && mock_dma_prepared == 0);
    clean();
}
static void failed_completion(bool unpublished) {
    start(); void *cpu = nullptr; uint64_t iova = 0;
    if (!unpublished) cpu = allocate();
    const auto cursor = log_tail();
    mock_complete_failure = true;
    if (unpublished) {
        mock_short_segment = true;
        assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0);
    } else {
        assert(dext_dma_free_coherent(cpu, 16384) != 0);
        assert(dext_dma_live_count() == 1);
        assert(dext_dma_free_coherent(cpu, 16384) != 0);
    }
    assert(mock_complete_calls == 1);
    assert(mock_dma_prepared == 1 && mock_objects == 2);
    assert(dext_dma_fini() != 0);
    assert(dext_dma_set_pci(&pci) != 0);
    assert(dext_dma_alloc_coherent(16384, &cpu, &iova) != 0);
    expect_dma_log(cursor, "DMA completion failed (0xffffffff); backing retained");
    expect_dma_log(cursor, "DMA fini rejected:");
    expect_dma_log(cursor, "cleanup_failed=1");
    /* Intentional quarantine owns both references until process termination. */
}
/* Build-226 close: upstream removal freed every DMA owner, but its BAR0
 * aperture mapping (left by amdgpu after drm_dev_unplug) blocked the reset. */
static void orphaned_bar0_alias() {
    start();
    void *ring = allocate(), *firmware = allocate();
    void *aperture = dext_bar0_cpu_map(0, 65536); assert(aperture);
    void *kmap = dext_bar0_cpu_map(4096, 4096); assert(kmap);
    assert(dext_bar0_live_count() == 2);
    assert(dext_dma_begin_shutdown(65536) == 0);
    /* Refused while any DMA owner or other CPU alias remains. */
    assert(dext_bar0_cpu_release_orphaned() == -1 && dext_bar0_live_count() == 2);
    assert(dext_dma_free_coherent(ring, 16384) == 0);
    void *pages = dext_cpu_alloc_pages(16384); assert(pages);
    assert(dext_dma_free_coherent(firmware, 16384) == 0);
    assert(dext_bar0_cpu_release_orphaned() == -1);
    assert(dext_cpu_free_pages(pages, 16384) == 0);
    auto cursor = log_tail();
    assert(dext_dma_begin_shutdown_reset() != 0);
    expect_dma_log(cursor, "DMA shutdown reset rejected:");
    expect_dma_log(cursor, "bars=2 cpu=0 vmaps=0 live=0 retired=2");
    const size_t objects = mock_objects;
    assert(dext_bar0_cpu_release_orphaned() == 2);
    assert(mock_objects < objects && dext_bar0_live_count() == 0);
    assert(!dext_bar0_cpu_contains(aperture, 1));
    assert(dext_bar0_cpu_release_orphaned() == 0);
    assert(dext_dma_begin_shutdown_reset() == 0);
    assert(dext_dma_end_shutdown_reset(1) == 0);
    clean();
    start(); /* the seam is reusable by the next session */
    clean();
    /* Never outside the shutdown hold. */
    start();
    void *bar = dext_bar0_cpu_map(0, 4096); assert(bar);
    assert(dext_bar0_cpu_release_orphaned() == -1 && dext_bar0_live_count() == 1);
    assert(dext_bar0_cpu_unmap(bar) == 1);
    clean();
}

/* A quiescent quarantine returns to the shutdown hold for a verified reset;
 * anything still owned keeps it in place. */
static void quarantine_release() {
    start();
    void *owned = allocate(), *retired = allocate();
    void *aperture = dext_bar0_cpu_map(0, 4096); assert(aperture);
    assert(dext_dma_begin_shutdown(65536) == 0);
    assert(dext_dma_free_coherent(retired, 16384) == 0);
    dext_dma_quarantine();
    assert(!dext_dma_quarantine_releasable());
    assert(dext_dma_lift_quarantine(1) != 0);
    /* A free that arrives during quarantine still retires its backing. */
    assert(dext_dma_free_coherent(owned, 16384) != 0);
    assert(dext_dma_quarantine_releasable());
    assert(dext_dma_lift_quarantine(0) != 0); /* retired backing needs a reset */
    const size_t completions = mock_complete_calls;
    assert(dext_dma_lift_quarantine(1) == 0);
    assert(!dext_dma_quarantine_releasable());
    void *unused = nullptr; uint64_t iova = 0;
    assert(dext_dma_alloc_coherent(16384, &unused, &iova) != 0);
    assert(dext_bar0_cpu_release_orphaned() == 1);
    assert(dext_dma_begin_shutdown_reset() == 0);
    assert(dext_dma_end_shutdown_reset(1) == 0);
    assert(mock_complete_calls == completions + 2);
    clean();

    /* Failed completion is never releasable. */
    start(); void *cpu = allocate();
    mock_complete_failure = true;
    assert(dext_dma_free_coherent(cpu, 16384) != 0);
    mock_complete_failure = false;
    dext_dma_quarantine();
    assert(!dext_dma_quarantine_releasable() && dext_dma_lift_quarantine(1) != 0);
    /* Intentional quarantine owns both references until process termination. */
}

int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "shutdown-reset-failed")) shutdown_failure(false);
        else if (!strcmp(argv[1], "shutdown-complete-failed")) shutdown_failure(true);
        else if (!strcmp(argv[1], "probe-commit-failed")) probe_commit_failure();
        else if (!strcmp(argv[1], "quarantine-prepare")) quarantine_inflight(false);
        else if (!strcmp(argv[1], "quarantine-complete")) quarantine_inflight(true);
        else if (!strcmp(argv[1], "orphaned-bar0")) orphaned_bar0_alias();
        else if (!strcmp(argv[1], "quarantine-release")) quarantine_release();
        else failed_completion(strcmp(argv[1], "unpublished") == 0);
    } else {
        reset_admission(); shutdown_retention(); probe_lifetime(); normal_and_rpc_failures(); stopping_during_allocation(); vmap_unwind(); cpu_pages_and_aliases();
        ranges_descriptor();
        address_widths(); platform_probe();
    }
    puts("production DriverKit DMA bridge offline failure checks passed");
}
