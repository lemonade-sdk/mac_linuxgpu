#pragma once
// Code objects' GPU memory: a heap of larger VRAM allocations (chunks), each
// carved into aligned ranges, one per loaded code object, as ROCr's loader
// sub-allocates code segments from its code pools rather than giving every
// code object a buffer of its own. A load costs no driver call: the image is
// relocated to its range and staged in a host-mapped buffer, and everything
// staged is uploaded before the next doorbell of the connection's queues
// (flushPendingCodeSync), each contiguous run by one SDMA copy, ahead of the
// one code-cache sync that load owes (kCodeSyncDriverBuild).
#include "transport.h"
#include <map>
#include <mutex>
#include <vector>

namespace mac_hsa {

// Ranges of chunks, first by best fit, freed ranges coalesced and reused.
// Bookkeeping only (no driver, no GPU): offline tests drive it directly.
// Sizes are rounded up to kCodeRangeGranule, so ranges handed out one after
// another in a chunk are adjacent (their uploads merge into one copy).
class CodeRangeAllocator {
public:
    static constexpr uint64_t kGranule = 4096;
    struct Range { size_t chunk = SIZE_MAX; uint64_t offset = 0, size = 0; };
    // A chunk of @size bytes at device address @base; its index (an index
    // of a removed chunk is reused).
    size_t addChunk(uint64_t base, uint64_t size);
    // A range of at least @size bytes whose device address is a multiple
    // of @alignment (a power of two), from the chunk where it leaves the
    // least free space; false when no chunk has room.
    bool allocate(uint64_t size, uint64_t alignment, Range &out);
    void free(const Range &range);
    bool chunkEmpty(size_t chunk) const;
    // Only an empty chunk is removed.
    bool removeChunk(size_t chunk);
    size_t liveChunks() const;
    uint64_t chunkBase(size_t chunk) const { return chunks[chunk].base; }
    uint64_t chunkSize(size_t chunk) const { return chunks[chunk].size; }
    uint64_t usedBytes() const;
    uint64_t freeBytes() const;
    // Free ranges across live chunks (fragmentation: one per chunk when
    // every free byte is contiguous).
    size_t freeRanges() const;
    static uint64_t roundUp(uint64_t value, uint64_t multiple) {
        return value > UINT64_MAX - (multiple - 1) ? 0 : (value + multiple - 1) / multiple * multiple;
    }
private:
    struct Chunk {
        uint64_t base = 0, size = 0, used = 0;
        bool live = false;
        std::map<uint64_t, uint64_t> free; // offset -> bytes
    };
    std::vector<Chunk> chunks;
};

// One connection's code heap. Thread safe; the connection outlives it (each
// loaded image holds both).
class CodeHeap {
public:
    // Standard chunk: images that fit share one; a larger image gets a
    // chunk of its own, sized to it.
    static constexpr uint64_t kChunkBytes = 4ull << 20;
    // Host-mapped staging for uploads (at most the driver's copy limit).
    static constexpr uint64_t kStagingBytes = 4ull << 20;
    struct Placement {
        DeviceBuffer chunk;   // the chunk's driver buffer
        uint64_t offset = 0, size = 0;
        CodeRangeAllocator::Range range;
        uint64_t address() const { return chunk.address + offset; }
    };
    struct Stats { size_t chunks = 0, ranges = 0, freeRanges = 0; uint64_t usedBytes = 0, freeBytes = 0, uploads = 0, staged = 0; };
    explicit CodeHeap(Connection &connection) : connection(connection) {}
    CodeHeap(const CodeHeap &) = delete;
    CodeHeap &operator=(const CodeHeap &) = delete;
    // The device as the first load read it: the ISA and driver build do
    // not change for a connection (a lost device fails its calls instead).
    hsa_status_t device(DeviceSnapshot &snapshot, IsaTarget &isa);
    // A range of @size bytes aligned to @alignment, the heap growing by a
    // chunk when no chunk has room. Fails, saying why on stderr, when the
    // driver refuses the chunk.
    hsa_status_t reserve(uint64_t size, uint64_t alignment, Placement &out);
    // @bytes of @data to land at @placement (bytes <= placement.size; the
    // rest of the range is padding, zeroed when staged): staged, uploaded
    // by flush. An image larger than the staging buffer is uploaded at
    // once, after everything staged before it.
    hsa_status_t stage(const Placement &placement, const void *data, size_t bytes);
    // Uploads everything staged: one copy for each run of adjacent ranges.
    // Staged runs stay pending when a copy fails.
    hsa_status_t flush();
    bool pending();
    // The range goes back to the heap; a chunk left empty is released to the
    // driver, but for one standard chunk kept for the next loads.
    void release(const Placement &placement);
    Stats stats();
private:
    struct Run { size_t chunk; uint64_t offset, staging, bytes; };
    Connection &connection;
    std::mutex mutex;
    CodeRangeAllocator allocator;
    std::vector<DeviceBuffer> chunks; // by allocator chunk index
    SharedBuffer staging;
    uint64_t stagingUsed = 0;
    std::vector<Run> runs;
    size_t ranges = 0;
    uint64_t uploads = 0, staged = 0;
    bool haveDevice = false;
    DeviceSnapshot snapshot;
    IsaTarget isa;
    hsa_status_t flushLocked();
    hsa_status_t growLocked(uint64_t size, uint64_t alignment);
};

} // namespace mac_hsa
