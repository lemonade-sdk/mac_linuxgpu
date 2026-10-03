// Host unit test: the allocation census (src/allocation_census.h) groups live
// buffers by the call that allocated them, sizes them, and forgets them when
// they are freed.
#include "allocation_census.h"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

using mac_hsa::AllocationCensus;

__attribute__((noinline)) void weightsSite(AllocationCensus &c, uint64_t handle) {
    c.add(handle, AllocationCensus::Kind::VRAM, 512ull << 20, 0);
}
__attribute__((noinline)) void slabSite(AllocationCensus &c, uint64_t handle) {
    c.add(handle, AllocationCensus::Kind::VRAM, 64ull << 20, 0);
}

int main() {
    AllocationCensus c;
    for (uint64_t h = 1; h <= 3; ++h) weightsSite(c, h);
    for (uint64_t h = 10; h < 20; ++h) slabSite(c, h);
    c.add(100, AllocationCensus::Kind::Shared, 16384);
    CHECK(c.count(AllocationCensus::Kind::VRAM) == 13, "13 VRAM buffers");
    CHECK(c.bytes(AllocationCensus::Kind::VRAM) == (3 * 512ull + 10 * 64ull) << 20, "VRAM bytes");
    CHECK(c.count(AllocationCensus::Kind::Shared) == 1 && c.bytes(AllocationCensus::Kind::Shared) == 16384,
          "shared counted apart");
    const std::string report = c.report();
    std::fputs(report.c_str(), stderr);
    CHECK(report.find("13 VRAM buffers") != std::string::npos, "report totals");
    CHECK(report.find("=64M 10 (640.0 MiB)") != std::string::npos, "64 MiB slabs bucketed");
    CHECK(report.find(">=512M 3 (1536.0 MiB)") != std::string::npos, "large buffers bucketed");
    CHECK(report.find("3 VRAM buffers, 1536.0 MiB, from") != std::string::npos &&
          report.find("10 VRAM buffers, 640.0 MiB, from") != std::string::npos,
          "grouped by allocating call, largest first");
    CHECK(report.find("1536.0 MiB, from") < report.find("640.0 MiB, from"), "largest group first");
    for (uint64_t h = 10; h < 20; ++h) c.remove(h);
    c.remove(999);
    CHECK(c.count(AllocationCensus::Kind::VRAM) == 3, "freed buffers leave the census");
    std::fprintf(stderr, failures ? "%d FAILURE(S)\n" : "all census checks passed\n", failures);
    return failures ? 1 : 0;
}
