#include "allocation_census.h"
#include <dlfcn.h>
#include <execinfo.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mac_hsa {

void AllocationCensus::add(uint64_t handle, Kind kind, uint64_t bytes, unsigned skip) {
    Record record;
    record.kind = kind;
    record.bytes = bytes;
    std::array<void *, kFrames + 8> frames{};
    const int depth = backtrace(frames.data(), int(frames.size()));
    const unsigned first = std::min<unsigned>(skip + 1, unsigned(std::max(depth, 0)));
    for (int i = int(first); i < depth && record.depth < kFrames; ++i)
        record.frames[record.depth++] = frames[size_t(i)];
    live[handle] = record;
    if (tracing()) trace("alloc", handle, record);
}

void AllocationCensus::remove(uint64_t handle) {
    const auto found = live.find(handle);
    if (found == live.end()) return;
    if (tracing()) trace("free", handle, found->second);
    live.erase(found);
}

uint64_t AllocationCensus::count(Kind kind) const {
    uint64_t n = 0;
    for (const auto &[handle, r] : live) { (void)handle; n += r.kind == kind; }
    return n;
}

uint64_t AllocationCensus::bytes(Kind kind) const {
    uint64_t n = 0;
    for (const auto &[handle, r] : live) { (void)handle; if (r.kind == kind) n += r.bytes; }
    return n;
}

bool AllocationCensus::tracing() {
    static const bool on = [] { const char *v = std::getenv("MAC_HSA_VRAM_TRACE"); return v && v[0] == '1'; }();
    return on;
}

std::string describeFrame(void *address) {
    Dl_info info{};
    char text[256];
    if (!dladdr(address, &info)) {
        std::snprintf(text, sizeof(text), "%p", address);
        return text;
    }
    const char *image = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
    image = image ? image + 1 : (info.dli_fname ? info.dli_fname : "?");
    const auto at = uintptr_t(address);
    // A symbol far below the address is the nearest export, not the function.
    if (info.dli_sname && info.dli_saddr && at - uintptr_t(info.dli_saddr) < 0x4000)
        std::snprintf(text, sizeof(text), "%s+%#" PRIxPTR, info.dli_sname, at - uintptr_t(info.dli_saddr));
    else
        std::snprintf(text, sizeof(text), "%s+%#" PRIxPTR, image, at - uintptr_t(info.dli_fbase));
    return text;
}

void AllocationCensus::trace(const char *what, uint64_t handle, const Record &record) const {
    std::string frames;
    for (unsigned i = 0; i < record.depth && i < 6; ++i) frames += " " + describeFrame(record.frames[i]);
    std::fprintf(stderr, "mac_hsa: %s %s handle=%#" PRIx64 " bytes=%" PRIu64 " live=%" PRIu64 "/%" PRIu64 "B |%s\n",
                 record.kind == Kind::VRAM ? "vram" : "shared", what, handle, record.bytes,
                 count(record.kind), bytes(record.kind), frames.c_str());
}

std::string AllocationCensus::report(unsigned callers) const {
    std::string out;
    char line[512];
    std::snprintf(line, sizeof(line), "mac_hsa: census: %" PRIu64 " VRAM buffers (%" PRIu64 " bytes), %" PRIu64
                  " shared buffers (%" PRIu64 " bytes)\n", count(Kind::VRAM), bytes(Kind::VRAM),
                  count(Kind::Shared), bytes(Kind::Shared));
    out += line;
    struct Bucket { const char *name; uint64_t below, count = 0, bytes = 0; };
    std::array<Bucket, 7> buckets{{{"<1M", 1ull << 20}, {"<4M", 4ull << 20}, {"<16M", 16ull << 20},
        {"<64M", 64ull << 20}, {"=64M", (64ull << 20) + 1}, {"<512M", 512ull << 20}, {">=512M", UINT64_MAX}}};
    for (const auto &[handle, r] : live) {
        (void)handle;
        if (r.kind != Kind::VRAM) continue;
        for (auto &b : buckets) if (r.bytes < b.below) { ++b.count; b.bytes += r.bytes; break; }
    }
    out += "mac_hsa: census: VRAM by size:";
    for (const auto &b : buckets) {
        if (!b.count) continue;
        std::snprintf(line, sizeof(line), " %s %" PRIu64 " (%.1f MiB)", b.name, b.count, double(b.bytes) / double(1 << 20));
        out += line;
    }
    out += "\n";
    // Group by the frames above the runtime's own: the first four recorded.
    struct Group { uint64_t count = 0, bytes = 0; const Record *sample = nullptr; };
    std::map<std::array<void *, 4>, Group> groups;
    for (const auto &[handle, r] : live) {
        (void)handle;
        if (r.kind != Kind::VRAM) continue;
        std::array<void *, 4> key{};
        for (unsigned i = 0; i < 4 && i < r.depth; ++i) key[i] = r.frames[i];
        auto &g = groups[key];
        ++g.count; g.bytes += r.bytes;
        if (!g.sample) g.sample = &r;
    }
    std::vector<const std::pair<const std::array<void *, 4>, Group> *> order;
    for (const auto &entry : groups) order.push_back(&entry);
    std::sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->second.bytes > b->second.bytes; });
    unsigned shown = 0;
    for (const auto *entry : order) {
        if (shown++ == callers) break;
        const Group &g = entry->second;
        std::snprintf(line, sizeof(line), "mac_hsa: census: %" PRIu64 " VRAM buffers, %.1f MiB, from", g.count,
                      double(g.bytes) / double(1 << 20));
        out += line;
        for (unsigned i = 0; i < g.sample->depth; ++i) out += " " + describeFrame(g.sample->frames[i]);
        out += "\n";
    }
    return out;
}

} // namespace mac_hsa
