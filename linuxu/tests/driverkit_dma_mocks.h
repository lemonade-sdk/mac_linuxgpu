/* DriverKit API substitutes for the production DMA bridge; no device access. */
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <functional>
#include <map>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

using kern_return_t = int;
constexpr int kIOReturnSuccess = 0;
constexpr int kIOReturnNoMemory = (int)0xe00002bd;
constexpr int kIOReturnNoResources = (int)0xe00002be;
constexpr int kIOMemoryDirectionOutIn = 0;
constexpr int kIODMACommandSpecificationNoOptions = 0;
constexpr int kIODMACommandCreateNoOptions = 0;
constexpr int kIODMACommandPrepareForDMANoOptions = 0;
constexpr int kIODMACommandCompleteDMANoOptions = 0;
constexpr int kIOMemoryMapCacheModeInhibit = 0;
struct IOAddressSegment { uint64_t address, length; };
struct IODMACommandSpecification { uint64_t options, maxAddressBits; };

static size_t mock_objects, mock_dma_prepared, mock_api_calls;
static size_t mock_fail_api, mock_complete_calls, mock_fail_allocation;
static bool mock_complete_failure, mock_short_segment;
/* Imports (several segments requested): how many segments the DART splits
 * a mapping into, and whether one of them comes back misaligned. */
static uint32_t mock_import_segments = 1;
static bool mock_import_misaligned;
/* The DART modeled by IODMACommand: it places every mapping at
 * mock_dart_iova, and (when nonzero) refuses PrepareForDMA for commands
 * narrower than mock_dart_refuse_below_bits. */
static uint64_t mock_dart_iova = 0x100000;
static uint64_t mock_dart_refuse_below_bits;
/* The DART's IOVA window: when nonzero, PrepareForDMA refuses a mapping
 * (kIOReturnNoResources) that would not fit beside those still prepared. */
static uint64_t mock_dart_window, mock_dart_mapped;
static uint64_t mock_last_address_bits, mock_dma_commands;
static std::map<void *, size_t> mock_allocations;
static std::function<void()> mock_prepare_hook, mock_complete_hook, mock_map_hook, mock_address_hook;
static bool mock_fail() { return ++mock_api_calls == mock_fail_api; }
static void mock_hook(std::function<void()> &hook) {
    auto run = hook; hook = {}; if (run) run();
}
static void *IOMalloc(size_t size) {
    if (mock_fail_allocation && --mock_fail_allocation == 0) return nullptr;
    void *p = malloc(size); assert(p);
    assert(mock_allocations.emplace(p, size).second); return p;
}
static void IOFree(void *p, size_t size) {
    auto it = mock_allocations.find(p);
    assert(it != mock_allocations.end() && it->second == size);
    mock_allocations.erase(it); free(p);
}
/* The dext heap (linuxu/src/shims/dext_alloc.c) where a test does not
 * link it: the host's. */
extern "C" __attribute__((weak)) void *linuxu_dext_malloc(size_t size) { return malloc(size); }
extern "C" __attribute__((weak)) void linuxu_dext_free(void *pointer) { free(pointer); }
#define IOLog(...) ((void)0)
static void IOSleep(unsigned) {}

class IOService {};
class IOPCIDevice : public IOService {
public:
    void ConfigurationRead32(uint64_t offset, uint32_t *value) {
        assert(offset < 4096 && !(offset & 3));
        ++mock_api_calls;
        *value = 0x744c1002;
    }
    void ConfigurationWrite32(uint64_t offset, uint32_t) {
        assert(offset < 4096 && !(offset & 3));
        ++mock_api_calls;
    }
    /* Narrow accessors model a function that has left config space. */
    void ConfigurationRead16(uint64_t offset, uint16_t *value) {
        assert(offset < 4096 && !(offset & 1));
        ++mock_api_calls;
        *value = UINT16_MAX;
    }
    void ConfigurationRead8(uint64_t offset, uint8_t *value) {
        assert(offset < 4096);
        ++mock_api_calls;
        *value = UINT8_MAX;
    }
    void ConfigurationWrite16(uint64_t offset, uint16_t) {
        assert(offset < 4096 && !(offset & 1));
        ++mock_api_calls;
    }
    void retain() {}
    void release() {}
};
class MockObject {
    unsigned refs = 1;
public:
    MockObject() { ++mock_objects; }
    virtual ~MockObject() { assert(refs == 0); --mock_objects; }
    void retain() { assert(refs); ++refs; }
    void release() { assert(refs); if (--refs == 0) delete this; }
};
class IOMemoryMap;
class IOMemoryDescriptor : public MockObject {
public:
	struct Segment { int fd; uint64_t offset, length; };
    uint64_t length = 0;
    void *bytes = nullptr;
	int owned_fd = -1;
	std::vector<Segment> segments;
    std::vector<IOMemoryDescriptor *> children;
    ~IOMemoryDescriptor() override {
        for (auto *child : children) child->release();
		if (bytes) { assert(!munmap(bytes, length)); assert(owned_fd >= 0); close(owned_fd); }
    }
    kern_return_t CreateMapping(uint64_t, uint64_t, uint64_t, uint64_t size,
                                uint64_t, IOMemoryMap **out);
    static kern_return_t CreateSubMemoryDescriptor(uint64_t, uint64_t offset,
            uint64_t size, IOMemoryDescriptor *parent, IOMemoryDescriptor **out) {
        *out = nullptr; if (mock_fail()) return -1;
        assert(offset <= parent->length && size <= parent->length - offset);
        auto *d = new IOMemoryDescriptor;
        d->length = size; parent->retain(); d->children.push_back(parent);
		uint64_t remaining = size;
		for (const auto &segment : parent->segments) {
			if (offset >= segment.length) { offset -= segment.length; continue; }
			uint64_t n = remaining < segment.length - offset ? remaining : segment.length - offset;
			d->segments.push_back({segment.fd, segment.offset + offset, n});
			remaining -= n; offset = 0;
			if (!remaining) break;
		}
		assert(!remaining);
        *out = d; return 0;
    }
    static kern_return_t CreateWithMemoryDescriptors(uint64_t, uint32_t count,
            IOMemoryDescriptor **children, IOMemoryDescriptor **out) {
        *out = nullptr; if (mock_fail()) return -1;
        assert(count && count <= 32);
        auto *d = new IOMemoryDescriptor;
        for (uint32_t i = 0; i < count; ++i) {
            children[i]->retain(); d->children.push_back(children[i]);
            d->length += children[i]->length;
			for (const auto &segment : children[i]->segments) d->segments.push_back(segment);
        }
        *out = d; return 0;
    }
};
class IOMemoryMap : public MockObject {
    IOMemoryDescriptor *descriptor;
    void *bytes;
    uint64_t length;
public:
    IOMemoryMap(IOMemoryDescriptor *d, uint64_t n) : descriptor(d), length(n) {
		d->retain();
		bytes = mmap(nullptr, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		assert(bytes != MAP_FAILED);
		uint64_t offset = 0;
		for (const auto &segment : d->segments) {
			uint64_t amount = segment.length < n - offset ? segment.length : n - offset;
			assert(mmap(static_cast<char *>(bytes) + offset, amount, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED, segment.fd, segment.offset) == static_cast<char *>(bytes) + offset);
			offset += amount;
			if (offset == n) break;
		}
		assert(offset == n);
    }
    ~IOMemoryMap() override { assert(!munmap(bytes, length)); descriptor->release(); }
    uint64_t GetAddress() { return reinterpret_cast<uintptr_t>(bytes); }
    uint64_t GetLength() { return length; }
};
inline kern_return_t IOMemoryDescriptor::CreateMapping(uint64_t, uint64_t,
        uint64_t, uint64_t size, uint64_t, IOMemoryMap **out) {
    *out = nullptr; if (mock_fail()) return -1;
    *out = new IOMemoryMap(this, size ? size : length);
    mock_hook(mock_map_hook); return 0;
}
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static kern_return_t Create(uint64_t, uint64_t size, uint64_t align,
                                IOBufferMemoryDescriptor **out) {
        *out = nullptr; if (mock_fail()) return -1;
        auto *b = new IOBufferMemoryDescriptor;
		char path[] = "/tmp/linuxgpu-dma-mock.XXXXXX";
		b->owned_fd = mkstemp(path); assert(b->owned_fd >= 0);
		assert(!unlink(path) && !ftruncate(b->owned_fd, size));
		b->bytes = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->owned_fd, 0);
		assert(b->bytes != MAP_FAILED && !(reinterpret_cast<uintptr_t>(b->bytes) & (align - 1)));
		b->segments.push_back({b->owned_fd, 0, size});
        b->length = size; *out = b; return 0;
    }
    kern_return_t SetLength(uint64_t size) {
        if (mock_fail()) return -1;
        assert(size <= length); length = size; return 0;
    }
    kern_return_t GetAddressRange(IOAddressSegment *range) {
        if (mock_fail()) return -1;
		mock_hook(mock_address_hook);
        *range = {reinterpret_cast<uintptr_t>(bytes), length}; return 0;
    }
};
class IODMACommand : public MockObject {
    IOMemoryDescriptor *buffer = nullptr;
    uint64_t address_bits = 0, mapped_bytes = 0;
public:
    ~IODMACommand() override { assert(!buffer); }
    static kern_return_t Create(IOPCIDevice *pci, uint64_t,
                                IODMACommandSpecification *spec, IODMACommand **out) {
        *out = nullptr; if (mock_fail()) return -1;
        assert(pci && spec->maxAddressBits >= 32 && spec->maxAddressBits <= 64);
        mock_last_address_bits = spec->maxAddressBits; ++mock_dma_commands;
        auto *command = new IODMACommand;
        command->address_bits = spec->maxAddressBits;
        *out = command; return 0;
    }
    kern_return_t PrepareForDMA(uint64_t, IOMemoryDescriptor *b,
            uint64_t, uint64_t size, uint64_t *, uint32_t *count, IOAddressSegment *seg) {
        if (mock_fail()) return -1;
        /* The IIG declaration passes IOAddressSegment segments[32]. */
        assert(!buffer && *count >= 1 && *count <= 32);
        if (address_bits < mock_dart_refuse_below_bits) return -1;
        if (mock_dart_window && size > mock_dart_window - mock_dart_mapped)
            return kIOReturnNoResources;
        buffer = b; b->retain(); ++mock_dma_prepared;
        mapped_bytes = size; mock_dart_mapped += size;
        if (*count == 1) {
            *seg = {mock_dart_iova, mock_short_segment ? size - 1 : size};
        } else {
            /* Equal runs of whole 16 KiB pages, each at its own IOVA. */
            const uint32_t n = mock_import_segments < *count ? mock_import_segments : *count;
            const uint64_t pages = size / 16384;
            uint64_t done = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const uint64_t run = i + 1 == n ? pages - done : pages / n;
                seg[i] = {mock_dart_iova + i * 0x1000000ULL, run * 16384};
                done += run;
            }
            if (mock_import_misaligned) seg[n - 1].address += 4096;
            *count = n;
        }
        mock_hook(mock_prepare_hook); return 0;
    }
    kern_return_t CompleteDMA(uint64_t) {
        ++mock_complete_calls; assert(buffer);
        mock_hook(mock_complete_hook);
        if (mock_complete_failure) return -1;
        mock_dart_mapped -= mapped_bytes; mapped_bytes = 0;
        buffer->release(); buffer = nullptr; --mock_dma_prepared; return 0;
    }
};