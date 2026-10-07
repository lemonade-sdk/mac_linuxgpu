#include "code_heap.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mac_hsa {

std::shared_ptr<CodeHeap> Connection::codeHeap() {
    std::lock_guard lock(codeHeapMutex);
    if (!codeHeap_) codeHeap_ = std::make_shared<CodeHeap>(*this);
    return codeHeap_;
}

size_t CodeRangeAllocator::addChunk(uint64_t base, uint64_t size) {
    size_t index = 0;
    while (index < chunks.size() && chunks[index].live) ++index;
    if (index == chunks.size()) chunks.emplace_back();
    auto &chunk = chunks[index];
    chunk = {};
    chunk.base = base; chunk.size = size; chunk.live = true;
    chunk.free.emplace(0, size);
    return index;
}

bool CodeRangeAllocator::allocate(uint64_t size, uint64_t alignment, Range &out) {
    if (!size || !alignment || (alignment & (alignment - 1))) return false;
    size = roundUp(size, kGranule);
    if (!size) return false;
    // Best fit: the free range that leaves the least, lowest chunk and
    // offset first among equals.
    size_t bestChunk = SIZE_MAX;
    uint64_t bestOffset = 0, bestLength = UINT64_MAX, bestStart = 0;
    for (size_t c = 0; c < chunks.size(); ++c) {
        const auto &chunk = chunks[c];
        if (!chunk.live || chunk.size - chunk.used < size) continue;
        for (const auto &[offset, length] : chunk.free) {
            if (length < size || length >= bestLength) continue;
            const auto aligned = roundUp(chunk.base + offset, alignment);
            if (!aligned) continue;
            const auto start = aligned - chunk.base;
            if (start - offset > length - size) continue;
            bestChunk = c; bestOffset = offset; bestLength = length; bestStart = start;
        }
    }
    if (bestChunk == SIZE_MAX) return false;
    auto &chunk = chunks[bestChunk];
    chunk.free.erase(bestOffset);
    if (bestStart > bestOffset) chunk.free.emplace(bestOffset, bestStart - bestOffset);
    const auto tail = bestOffset + bestLength - (bestStart + size);
    if (tail) chunk.free.emplace(bestStart + size, tail);
    chunk.used += size;
    out = {bestChunk, bestStart, size};
    return true;
}

void CodeRangeAllocator::free(const Range &range) {
    if (range.chunk >= chunks.size() || !chunks[range.chunk].live || !range.size) return;
    auto &chunk = chunks[range.chunk];
    uint64_t offset = range.offset, size = range.size;
    auto next = chunk.free.lower_bound(offset);
    if (next != chunk.free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) {
            offset = previous->first; size += previous->second;
            chunk.free.erase(previous);
        }
    }
    if (next != chunk.free.end() && range.offset + range.size == next->first) {
        size += next->second;
        chunk.free.erase(next);
    }
    chunk.free.emplace(offset, size);
    chunk.used -= std::min(chunk.used, range.size);
}

bool CodeRangeAllocator::chunkEmpty(size_t chunk) const {
    return chunk < chunks.size() && chunks[chunk].live && !chunks[chunk].used;
}

bool CodeRangeAllocator::removeChunk(size_t chunk) {
    if (!chunkEmpty(chunk)) return false;
    chunks[chunk] = {};
    return true;
}

size_t CodeRangeAllocator::liveChunks() const {
    return size_t(std::count_if(chunks.begin(), chunks.end(), [](const Chunk &c) { return c.live; }));
}

uint64_t CodeRangeAllocator::usedBytes() const {
    uint64_t used = 0;
    for (const auto &chunk : chunks) if (chunk.live) used += chunk.used;
    return used;
}

uint64_t CodeRangeAllocator::freeBytes() const {
    uint64_t free = 0;
    for (const auto &chunk : chunks) if (chunk.live) free += chunk.size - chunk.used;
    return free;
}

size_t CodeRangeAllocator::freeRanges() const {
    size_t count = 0;
    for (const auto &chunk : chunks) if (chunk.live) count += chunk.free.size();
    return count;
}

hsa_status_t CodeHeap::device(DeviceSnapshot &out, IsaTarget &outIsa) {
    std::lock_guard lock(mutex);
    if (!haveDevice) {
        DeviceSnapshot read;
        const auto status = connection.read(read);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!deviceIsa(read, isa)) return HSA_STATUS_ERROR_INVALID_ISA;
        snapshot = read; haveDevice = true;
    }
    out = snapshot; outIsa = isa;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t CodeHeap::growLocked(uint64_t size, uint64_t alignment) {
    // A standard chunk holds whatever fits with its alignment; a larger
    // image gets one of its own (the driver aligns buffers to 16 KiB).
    const uint64_t padding = alignment > 16384 ? alignment : 0;
    const uint64_t bytes = size + padding <= kChunkBytes ? kChunkBytes :
        CodeRangeAllocator::roundUp(size + padding, 16384);
    DeviceBuffer buffer;
    const auto status = bytes ? connection.allocateBuffer(bytes, buffer) : HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (status != HSA_STATUS_SUCCESS || !buffer.handle || !buffer.address || buffer.size < bytes ||
        buffer.address >= (1ull << 48) || buffer.size > (1ull << 48) - buffer.address) {
        std::fprintf(stderr, "mac_hsa: the code-object heap could not grow by %#llx bytes of VRAM "
            "(%zu chunks, %#llx bytes holding %zu code objects): the driver %s (status %#x)\n",
            (unsigned long long)bytes, allocator.liveChunks(), (unsigned long long)allocator.usedBytes(), ranges,
            status == HSA_STATUS_SUCCESS ? "returned an unusable buffer" : "refused the allocation", unsigned(status));
        if (status == HSA_STATUS_SUCCESS && buffer.handle) (void)connection.freeBuffer(buffer);
        return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR_OUT_OF_RESOURCES : status;
    }
    try {
        const auto index = allocator.addChunk(buffer.address, buffer.size);
        if (index >= chunks.size()) chunks.resize(index + 1);
        chunks[index] = buffer;
    } catch (const std::bad_alloc &) {
        (void)connection.freeBuffer(buffer);
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t CodeHeap::reserve(uint64_t size, uint64_t alignment, Placement &out) {
    out = {};
    alignment = std::max<uint64_t>(alignment, CodeRangeAllocator::kGranule);
    if (!size || (alignment & (alignment - 1)) || size > (256ull << 20) || alignment > (256ull << 20))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex);
    CodeRangeAllocator::Range range;
    if (!allocator.allocate(size, alignment, range)) {
        const auto status = growLocked(CodeRangeAllocator::roundUp(size, CodeRangeAllocator::kGranule), alignment);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!allocator.allocate(size, alignment, range)) {
            std::fprintf(stderr, "mac_hsa: the code-object heap grew but %#llx bytes aligned to %#llx "
                "still do not fit\n", (unsigned long long)size, (unsigned long long)alignment);
            return HSA_STATUS_ERROR;
        }
    }
    out = {chunks[range.chunk], range.offset, range.size, range};
    ++ranges;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t CodeHeap::stage(const Placement &placement, const void *data, size_t bytes) {
    if (!data || !bytes || bytes > placement.size || !placement.chunk.handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex);
    if (!staging.host) {
        const auto capacity = std::min<uint64_t>(kStagingBytes, connection.maxCopyBytes());
        const auto status = connection.allocateSharedBuffer(capacity, staging);
        if (status != HSA_STATUS_SUCCESS || !staging.host || staging.device.size < capacity) {
            std::fprintf(stderr, "mac_hsa: no host-mapped staging buffer (%#llx bytes) for code-object "
                "uploads (status %#x)\n", (unsigned long long)capacity, unsigned(status));
            if (status == HSA_STATUS_SUCCESS && staging.host) (void)connection.freeSharedBuffer(staging);
            staging = {};
            return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR_OUT_OF_RESOURCES : status;
        }
        stagingUsed = 0;
    }
    if (placement.size > staging.device.size) {
        // Larger than the staging buffer: uploaded now, after everything
        // staged before it (a staged run may cover a range this one reuses).
        auto status = flushLocked();
        if (status == HSA_STATUS_SUCCESS) status = connection.writeBuffer(placement.chunk, placement.offset, data, bytes);
        if (status == HSA_STATUS_SUCCESS) ++uploads;
        return status;
    }
    if (placement.size > staging.device.size - stagingUsed) {
        const auto status = flushLocked();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (placement.size > staging.device.size - stagingUsed) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    auto *at = static_cast<uint8_t *>(staging.host) + stagingUsed;
    std::memcpy(at, data, bytes);
    std::memset(at + bytes, 0, size_t(placement.size - bytes));
    const auto chunk = placement.range.chunk;
    if (!runs.empty() && runs.back().chunk == chunk && runs.back().offset + runs.back().bytes == placement.offset &&
        runs.back().staging + runs.back().bytes == stagingUsed && runs.back().bytes <= UINT64_MAX - placement.size) {
        runs.back().bytes += placement.size;
    } else {
        try { runs.push_back({chunk, placement.offset, stagingUsed, placement.size}); }
        catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    }
    stagingUsed += placement.size;
    ++staged;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t CodeHeap::flushLocked() {
    size_t done = 0;
    hsa_status_t status = HSA_STATUS_SUCCESS;
    for (; done < runs.size(); ++done) {
        const auto &run = runs[done];
        status = connection.copyBuffers(staging.device, run.staging, chunks[run.chunk], run.offset, size_t(run.bytes));
        if (status != HSA_STATUS_SUCCESS) break;
        ++uploads;
    }
    runs.erase(runs.begin(), runs.begin() + ptrdiff_t(done));
    if (runs.empty()) stagingUsed = 0;
    return status;
}

hsa_status_t CodeHeap::flush() {
    std::lock_guard lock(mutex);
    return flushLocked();
}

bool CodeHeap::pending() {
    std::lock_guard lock(mutex);
    return !runs.empty();
}

void CodeHeap::release(const Placement &placement) {
    std::lock_guard lock(mutex);
    const auto index = placement.range.chunk;
    if (index >= chunks.size() || chunks[index].handle != placement.chunk.handle) return;
    allocator.free(placement.range);
    if (ranges) --ranges;
    if (!allocator.chunkEmpty(index)) return;
    // Keep one empty standard chunk for the next loads; release the rest.
    const bool standard = chunks[index].size == kChunkBytes;
    bool another = false;
    for (size_t c = 0; c < chunks.size(); ++c)
        if (c != index && chunks[c].handle && chunks[c].size == kChunkBytes && allocator.chunkEmpty(c)) another = true;
    if (standard && !another) return;
    // Its staged runs only cover ranges that are free now.
    std::erase_if(runs, [&](const Run &run) { return run.chunk == index; });
    if (runs.empty()) stagingUsed = 0;
    const auto status = connection.freeBuffer(chunks[index]);
    if (status != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "mac_hsa: releasing an empty code-object heap chunk %#llx+%#llx failed (status %#x); "
            "it stays in the heap\n", (unsigned long long)chunks[index].address,
            (unsigned long long)chunks[index].size, unsigned(status));
        return;
    }
    allocator.removeChunk(index);
    chunks[index] = {};
}

CodeHeap::Stats CodeHeap::stats() {
    std::lock_guard lock(mutex);
    return {allocator.liveChunks(), ranges, allocator.freeRanges(), allocator.usedBytes(), allocator.freeBytes(),
            uploads, staged};
}

} // namespace mac_hsa
