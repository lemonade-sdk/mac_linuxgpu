// Host unit test: the code-object heap (code_heap.h).
//
// 1. CodeRangeAllocator alone: alignment, best fit, reuse of freed ranges,
//    coalescing, fragmentation under a randomized load/unload mix checked
//    against a model (no overlap, every range inside its chunk, exact byte
//    accounting), chunk removal and index reuse.
// 2. CodeHeap against a fake connection: staged uploads land by one copy per
//    run of adjacent ranges; a full staging buffer flushes on its own; an
//    image larger than the staging buffer is written at once, after what
//    was staged before it; a reused range shows its new image; empty chunks
//    go back to the driver but for one standard chunk.
// 3. The loader (fake transport): hundreds of code objects share one chunk
//    and cost no driver call at load; one upload and one code sync before
//    the next doorbell; device bytes equal the relocated images; destroyed
//    executables' ranges are reused; a driver launch names the chunk and
//    the image's offset. With "legacy" (a driver before CodeSync) each load
//    uploads and syncs at once, as before.

#include "code_heap.h"
#include "code_object.h"
#include "mac_hsa.h"
#include "signal_kernels.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <hsa/hsa_ven_amd_loader.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", std::string(msg).c_str()); } \
} while (0)

using mac_hsa::CodeRangeAllocator;
using mac_hsa::CodeHeap;

// The fake's VRAM device address is its host pointer with bit 40 set.
static const uint8_t *fakeDevice(uint64_t address) {
    return reinterpret_cast<const uint8_t *>(uintptr_t(address & ~(1ull << 40)));
}

static void allocatorTests() {
    std::fprintf(stderr, "== CodeRangeAllocator ==\n");
    CodeRangeAllocator a;
    const auto c0 = a.addChunk(0x100000, 1 << 20);
    CodeRangeAllocator::Range r1, r2, r3, r4;
    CHECK(a.allocate(100, 4096, r1) && r1.chunk == c0 && r1.offset == 0 && r1.size == 4096,
          "a small range is rounded to the granule at the chunk start");
    CHECK(a.allocate(5000, 4096, r2) && r2.offset == 4096 && r2.size == 8192, "the next is adjacent");
    CHECK(a.allocate(4096, 65536, r3) && (0x100000 + r3.offset) % 65536 == 0 && r3.offset == 65536,
          "a 64 KiB alignment is met on the device address");
    CHECK(a.usedBytes() == 4096 + 8192 + 4096 && a.freeRanges() == 2, "the alignment gap stays free");
    CHECK(a.allocate(4096, 4096, r4) && r4.offset == 12288, "best fit takes the gap, not the tail");
    a.free(r2);
    CodeRangeAllocator::Range again;
    CHECK(a.allocate(8000, 4096, again) && again.offset == r2.offset, "a freed range is reused");
    a.free(again); a.free(r1); a.free(r4); a.free(r3);
    CHECK(a.freeRanges() == 1 && a.usedBytes() == 0 && a.chunkEmpty(c0), "freeing everything coalesces the chunk");
    CodeRangeAllocator::Range big;
    CHECK(!a.allocate(2 << 20, 4096, big), "nothing larger than every chunk");
    const auto c1 = a.addChunk(0x10000000, 4 << 20);
    CHECK(a.allocate(2 << 20, 4096, big) && big.chunk == c1, "a new chunk takes it");
    CHECK(!a.removeChunk(c1), "a chunk in use is not removed");
    CHECK(a.removeChunk(c0) && a.liveChunks() == 1, "an empty chunk is removed");
    CHECK(a.addChunk(0x200000, 1 << 20) == c0, "its index is reused");
    CHECK(!a.allocate(0, 4096, big) && !a.allocate(4096, 3, big), "zero size and non-power-of-two alignment refused");

    // Randomized load/unload against a model.
    CodeRangeAllocator b;
    std::mt19937_64 random(1234);
    for (int i = 0; i < 3; ++i) b.addChunk(0x1000000ull * (i + 1) + 0x4000, 4 << 20);
    std::vector<CodeRangeAllocator::Range> live;
    uint64_t modelUsed = 0;
    bool consistent = true;
    size_t refusals = 0;
    bool wrongRefusals = false;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || random() % 3) {
            const uint64_t size = 1 + random() % (random() % 8 ? 20000 : 300000);
            const uint64_t alignment = random() % 16 ? 4096 : 65536;
            CodeRangeAllocator::Range r;
            if (!b.allocate(size, alignment, r)) {
                // A refusal is genuine: no gap between live ranges fits.
                ++refusals;
                const auto rounded = CodeRangeAllocator::roundUp(size, 4096);
                for (size_t c = 0; c < 3; ++c) {
                    std::map<uint64_t, uint64_t> used;
                    for (const auto &other : live) if (other.chunk == c) used.emplace(other.offset, other.size);
                    uint64_t cursor = 0;
                    used.emplace(b.chunkSize(c), 0);
                    for (const auto &[offset, length] : used) {
                        const auto start = CodeRangeAllocator::roundUp(b.chunkBase(c) + cursor, alignment) - b.chunkBase(c);
                        if (start + rounded <= offset) wrongRefusals = true;
                        cursor = offset + length;
                    }
                }
                continue;
            }
            if (r.size < size || r.size % 4096 || (b.chunkBase(r.chunk) + r.offset) % alignment ||
                r.offset + r.size > b.chunkSize(r.chunk)) consistent = false;
            for (const auto &other : live)
                if (other.chunk == r.chunk && r.offset < other.offset + other.size && other.offset < r.offset + r.size)
                    consistent = false;
            live.push_back(r); modelUsed += r.size;
        } else {
            const auto index = random() % live.size();
            b.free(live[index]); modelUsed -= live[index].size;
            live[index] = live.back(); live.pop_back();
        }
        if (b.usedBytes() != modelUsed) consistent = false;
    }
    CHECK(consistent, "20000 random loads/unloads: no overlap, aligned, inside chunks, exact accounting");
    CHECK(refusals > 0 && !wrongRefusals,
          "every refusal (" + std::to_string(refusals) + ", the heap kept full) had no free range that fits");
    for (const auto &r : live) b.free(r);
    CHECK(b.usedBytes() == 0 && b.freeRanges() == 3, "every chunk coalesces back to one free range");
}

static void heapTests() {
    std::fprintf(stderr, "== CodeHeap ==\n");
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
    config.build = mac_hsa::kCodeSyncDriverBuild;
    mac_hsa::FakeConnection fake(config);
    CodeHeap heap(fake);
    std::vector<uint8_t> image(10000);
    for (size_t i = 0; i < image.size(); ++i) image[i] = uint8_t(i * 7 + 1);

    // Many small images: one chunk, staged, then one copy.
    std::vector<CodeHeap::Placement> placed(100);
    bool ok = true;
    for (size_t i = 0; i < placed.size(); ++i) {
        ok &= heap.reserve(image.size(), 4096, placed[i]) == HSA_STATUS_SUCCESS;
        image[0] = uint8_t(i);
        ok &= heap.stage(placed[i], image.data(), image.size()) == HSA_STATUS_SUCCESS;
    }
    CHECK(ok, "100 images reserved and staged");
    CHECK(fake.allocationCount() == 1 && fake.copyCount() == 0 && heap.pending(),
          "one chunk, nothing copied yet");
    CHECK(heap.flush() == HSA_STATUS_SUCCESS && fake.copyCount() == 1 && !heap.pending(),
          "one copy uploads all 100 (adjacent ranges)");
    bool landed = true;
    for (size_t i = 0; i < placed.size(); ++i) {
        image[0] = uint8_t(i);
        landed &= !std::memcmp(fakeDevice(placed[i].address()), image.data(), image.size());
        landed &= placed[i].address() % 4096 == 0;
    }
    CHECK(landed, "every image is at its address on the device");

    // A freed range reused before the flush shows its new image.
    heap.release(placed[10]);
    CodeHeap::Placement reused;
    CHECK(heap.reserve(image.size(), 4096, reused) == HSA_STATUS_SUCCESS && reused.address() == placed[10].address(),
          "the freed range is reserved again");
    std::vector<uint8_t> other(image.size(), 0xab);
    CHECK(heap.stage(reused, other.data(), other.size()) == HSA_STATUS_SUCCESS && heap.flush() == HSA_STATUS_SUCCESS &&
          !std::memcmp(fakeDevice(reused.address()), other.data(), other.size()), "and holds the new image");
    placed[10] = reused;

    // More than the staging buffer holds: it flushes on its own.
    const auto copies = fake.copyCount();
    std::vector<CodeHeap::Placement> many(500);
    ok = true;
    for (auto &p : many) {
        ok &= heap.reserve(image.size(), 4096, p) == HSA_STATUS_SUCCESS;
        ok &= heap.stage(p, image.data(), image.size()) == HSA_STATUS_SUCCESS;
    }
    CHECK(ok && heap.flush() == HSA_STATUS_SUCCESS, "500 more staged (more than the 4 MiB staging buffer)");
    const auto stats = heap.stats();
    CHECK(stats.chunks == 2 && fake.allocationCount() == 2, "two chunks hold 600 images");
    CHECK(fake.copyCount() - copies >= 2 && fake.copyCount() - copies <= 4,
          "uploaded in a few copies (" + std::to_string(fake.copyCount() - copies) + ")");
    landed = true;
    for (const auto &p : many) landed &= !std::memcmp(fakeDevice(p.address()) + 1, image.data() + 1, image.size() - 1);
    CHECK(landed, "every one landed");

    // A large image: a chunk of its own, written at once after the staged runs.
    std::vector<uint8_t> large(5 << 20, 0x5c);
    CodeHeap::Placement small, huge;
    CHECK(heap.reserve(image.size(), 4096, small) == HSA_STATUS_SUCCESS &&
          heap.stage(small, image.data(), image.size()) == HSA_STATUS_SUCCESS && heap.pending(), "a small one staged");
    CHECK(heap.reserve(large.size(), 4096, huge) == HSA_STATUS_SUCCESS && heap.stats().chunks == 3,
          "a 5 MiB image gets its own chunk");
    CHECK(heap.stage(huge, large.data(), large.size()) == HSA_STATUS_SUCCESS && !heap.pending() &&
          !std::memcmp(fakeDevice(huge.address()), large.data(), large.size()) &&
          !std::memcmp(fakeDevice(small.address()) + 1, image.data() + 1, image.size() - 1),
          "written at once, the staged one first");
    heap.release(huge);
    CHECK(heap.stats().chunks == 2 && fake.bufferCount() == 3, "its chunk goes back to the driver when freed");

    // Unload everything: one empty standard chunk stays.
    heap.release(small);
    for (const auto &p : placed) heap.release(p);
    for (const auto &p : many) heap.release(p);
    const auto after = heap.stats();
    CHECK(after.ranges == 0 && after.usedBytes == 0 && after.chunks == 1, "one empty chunk kept for the next loads");
    CHECK(fake.bufferCount() == 2, "the other went back (one chunk and the staging buffer remain)");
    CodeHeap::Placement next;
    CHECK(heap.reserve(image.size(), 4096, next) == HSA_STATUS_SUCCESS && fake.allocationCount() == 3,
          "the next load takes the kept chunk, no new allocation");
    heap.release(next);

    // A heap that cannot grow says so and fails.
    CodeHeap::Placement refused;
    CHECK(heap.reserve(300ull << 20, 4096, refused) == HSA_STATUS_ERROR_INVALID_ARGUMENT,
          "an image over the code-object limit is refused");
    fake.setPowerState(amdgpu::power::PowerState::Suspended);
    CodeHeap::Placement full;
    const auto fresh = heap.reserve(8 << 20, 4096, full);
    CHECK(fresh != HSA_STATUS_SUCCESS && !full.size, "a chunk the driver refuses fails the reservation");
    fake.setPowerState(amdgpu::power::PowerState::Active);
}

static hsa_agent_t findGPU() {
    hsa_agent_t gpu{};
    hsa_iterate_agents([](hsa_agent_t agent, void *data) -> hsa_status_t {
        hsa_device_type_t type;
        if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
            type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_SUCCESS;
    }, &gpu);
    return gpu;
}

static hsa_status_t load(hsa_agent_t gpu, std::span<const uint8_t> object, hsa_executable_t &executable) {
    hsa_code_object_reader_t reader{};
    auto status = hsa_code_object_reader_create_from_memory(object.data(), object.size(), &reader);
    if (status != HSA_STATUS_SUCCESS) return status;
    status = hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr, &executable);
    if (status == HSA_STATUS_SUCCESS) status = hsa_executable_load_agent_code_object(executable, gpu, reader, nullptr, nullptr);
    if (status == HSA_STATUS_SUCCESS) status = hsa_executable_freeze(executable, nullptr);
    hsa_code_object_reader_destroy(reader);
    return status;
}

static uint64_t kernelObject(hsa_executable_t executable, hsa_agent_t gpu, const std::string &name) {
    hsa_executable_symbol_t symbol{};
    uint64_t object = 0;
    if (hsa_executable_get_symbol_by_name(executable, name.c_str(), &gpu, &symbol) == HSA_STATUS_SUCCESS)
        hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &object);
    return object;
}

static void loaderTests(bool legacy) {
    std::fprintf(stderr, "== loader (%s) ==\n", legacy ? "driver before CodeSync" : "CodeSync driver");
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
    config.build = legacy ? mac_hsa::kQueueResourceDriverBuild : mac_hsa::kCodeSyncDriverBuild;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init");
    const auto gpu = findGPU();
    const auto fake = mac_hsa::fakeConnection();
    mac_hsa::IsaTarget isa;
    mac_hsa::SignalKernelObjects objects;
    mac_hsa::CodeObject parsed;
    CHECK(mac_hsa::resolveIsaTarget(120000, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa) &&
          mac_hsa::selectSignalKernels(isa, objects) && mac_hsa::parseCodeObject(objects.operations, parsed, isa) &&
          !parsed.kernels.empty(), "a code object and its kernel");
    if (!objects.operations.size() || parsed.kernels.empty()) return;
    const auto name = parsed.kernels[0].symbol;

    hsa_queue_t *queue = nullptr;
    CHECK(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue) == HSA_STATUS_SUCCESS, "a queue");
    const auto allocations = fake->allocationCount(), copies = fake->copyCount(), syncs = fake->codeSyncCount();
    constexpr size_t kLoads = 250; // 16 KiB each: one 4 MiB chunk
    std::vector<hsa_executable_t> executables(kLoads);
    bool loaded = true;
    for (auto &executable : executables) loaded &= load(gpu, objects.operations, executable) == HSA_STATUS_SUCCESS;
    CHECK(loaded, std::to_string(kLoads) + " code objects load, one executable each");
    CHECK(fake->allocationCount() - allocations == 1, "they share one chunk: one driver allocation");
    if (legacy) {
        CHECK(fake->copyCount() - copies == kLoads && fake->codeSyncCount() - syncs == kLoads,
              "a driver before CodeSync uploads and syncs each load");
    } else {
        CHECK(fake->copyCount() == copies && fake->codeSyncCount() == syncs, "no copy and no sync at load");
        hsa_signal_store_screlease(queue->doorbell_signal, 0);
        CHECK(fake->copyCount() - copies == 1 && fake->codeSyncCount() - syncs == 1,
              "the next doorbell: one copy uploads them all, then one sync");
        CHECK(fake->kicksAtLastCodeSync() == 0, "both ahead of that doorbell");
    }
    std::set<uint64_t> addresses;
    bool aligned = true, contents = true, hostMatches = true;
    for (auto executable : executables) {
        const auto object = kernelObject(executable, gpu, name);
        addresses.insert(object);
        aligned &= object % 64 == 0 && (object - parsed.kernels[0].descriptor) % std::max<uint64_t>(4096, parsed.alignment) == 0;
        const void *host = nullptr;
        hostMatches &= hsa_ven_amd_loader_query_host_address(reinterpret_cast<const void *>(object), &host) ==
            HSA_STATUS_SUCCESS && host;
        // The descriptor and the code, as relocated for this address.
        if (host) {
            const auto base = object - parsed.kernels[0].descriptor;
            const auto hostBase = static_cast<const uint8_t *>(host) - parsed.kernels[0].descriptor;
            for (const auto &segment : parsed.segments)
                contents &= !std::memcmp(fakeDevice(base + segment.offset), hostBase + segment.offset, segment.fileSize);
        }
        hsa_executable_t owner{};
        hostMatches &= hsa_ven_amd_loader_query_executable(reinterpret_cast<const void *>(object), &owner) ==
            HSA_STATUS_SUCCESS && owner.handle == executable.handle;
    }
    CHECK(addresses.size() == kLoads && aligned, "distinct kernel objects at their segments' alignment");
    CHECK(hostMatches, "address queries find each image and its executable");
    CHECK(contents, "the device holds every relocated image");

    // Unload half, load again: the freed ranges are reused, no new chunk.
    std::set<uint64_t> freed;
    for (size_t i = 0; i < kLoads; i += 2) {
        freed.insert(kernelObject(executables[i], gpu, name));
        hsa_executable_destroy(executables[i]);
    }
    const auto before = fake->allocationCount();
    bool reused = true;
    for (size_t i = 0; i < kLoads; i += 2) {
        reused &= load(gpu, objects.operations, executables[i]) == HSA_STATUS_SUCCESS;
        reused &= freed.count(kernelObject(executables[i], gpu, name)) == 1;
    }
    CHECK(reused && fake->allocationCount() == before, "reloads take the freed ranges, no new allocation");
    if (!legacy) {
        const auto syncsBefore = fake->codeSyncCount();
        hsa_signal_store_screlease(queue->doorbell_signal, 1);
        CHECK(fake->codeSyncCount() == syncsBefore + 1, "reused ranges are synced before the next doorbell");
    }
    // Unload everything: the chunk is kept, so the next load allocates nothing.
    for (auto executable : executables) hsa_executable_destroy(executable);
    hsa_executable_t last{};
    CHECK(load(gpu, objects.operations, last) == HSA_STATUS_SUCCESS && fake->allocationCount() == before,
          "after unloading everything the next load reuses the kept chunk");
    hsa_executable_destroy(last);
    CHECK(hsa_queue_destroy(queue) == HSA_STATUS_SUCCESS, "queue destroyed");
    hsa_shut_down();
}

static void launchTest() {
    std::fprintf(stderr, "== driver launch ==\n");
    mac_hsa::FakeDeviceConfig config;
    config.gcMajor = 12; config.gcMinor = 0; config.gcRevision = 0;
    config.build = mac_hsa::kCodeSyncDriverBuild;
    mac_hsa::setFakeDeviceConfig(config);
    CHECK(hsa_init() == HSA_STATUS_SUCCESS, "hsa_init");
    const auto gpu = findGPU();
    const auto fake = mac_hsa::fakeConnection();
    mac_hsa::IsaTarget isa;
    mac_hsa::SignalKernelObjects objects;
    mac_hsa::CodeObject parsed;
    mac_hsa::resolveIsaTarget(120000, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa);
    mac_hsa::selectSignalKernels(isa, objects);
    mac_hsa::parseCodeObject(objects.operations, parsed, isa);
    hsa_executable_t first{}, second{};
    CHECK(load(gpu, objects.operations, first) == HSA_STATUS_SUCCESS &&
          load(gpu, objects.operations, second) == HSA_STATUS_SUCCESS, "two loads");
    const auto &kernel = parsed.kernels[0];
    hsa_executable_symbol_t symbol{};
    hsa_executable_get_symbol_by_name(second, kernel.symbol.c_str(), &gpu, &symbol);
    const auto object = kernelObject(second, gpu, kernel.symbol);
    const auto syncs = fake->codeSyncCount(), copies = fake->copyCount();
    std::vector<uint8_t> args(kernel.kernargSize);
    const uint32_t one[3] = {1, 1, 1}, wave[3] = {32, 1, 1};
    uint64_t fence = 0;
    const auto status = mac_hsa_executable_dispatch_aql(symbol, args.data(), args.size(), one, wave, nullptr, 0, &fence);
    CHECK(fake->copyCount() == copies + 1 && fake->codeSyncCount() == syncs + 1,
          "a driver launch first uploads and syncs what is staged");
    const auto *aql = fake->lastAQL();
    CHECK(status == HSA_STATUS_SUCCESS && aql, "the launch reached the driver");
    if (aql) {
        // The chunk's device address plus the descriptor offset the
        // driver is given is the kernel object.
        const auto chunkBase = object - aql->descriptorOffset;
        CHECK(aql->descriptorOffset > kernel.descriptor && chunkBase % 16384 == 0,
              "it names the chunk and the image's offset in it");
    }
    hsa_executable_destroy(first); hsa_executable_destroy(second);
    hsa_shut_down();
}

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "legacy")) {
        loaderTests(true);
    } else {
        allocatorTests();
        heapTests();
        loaderTests(false);
        launchTest();
    }
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
