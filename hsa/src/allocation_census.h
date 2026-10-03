#pragma once

// Every buffer a connection holds from the driver, with where in the process
// it was asked for. The driver reports how much VRAM this process holds; the
// census says what holds it: for each live buffer its size and the return
// addresses of the call that allocated it, grouped by caller for a report.
//
// Frames are recorded raw and named only when a report is made. Names come
// from dladdr, which sees only exported symbols, so a frame in a statically
// linked library reports its image and offset instead; symbolicate those
// with atos against the image the report names.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace mac_hsa {

class AllocationCensus {
public:
    static constexpr size_t kFrames = 10;
    enum class Kind : uint8_t { VRAM, Shared };
    struct Record {
        Kind kind = Kind::VRAM;
        uint64_t bytes = 0;
        std::array<void *, kFrames> frames{};
        uint8_t depth = 0;
    };

    // Records a buffer under `handle`, capturing the caller's stack. `skip`
    // drops that many innermost frames (the runtime's own).
    void add(uint64_t handle, Kind kind, uint64_t bytes, unsigned skip = 1);
    void remove(uint64_t handle);
    void clear() { live.clear(); }

    uint64_t count(Kind kind) const;
    uint64_t bytes(Kind kind) const;
    // A multi-line report: totals by kind, a size histogram for VRAM, and the
    // callers holding the most VRAM with their frames. At most `callers`
    // caller groups.
    std::string report(unsigned callers = 12) const;
    // MAC_HSA_VRAM_TRACE=1: one line per buffer allocated and freed.
    static bool tracing();
    void trace(const char *what, uint64_t handle, const Record &record) const;

    const std::map<uint64_t, Record> &records() const { return live; }

private:
    std::map<uint64_t, Record> live;
};

// "symbol+0x12" or "image+0x1234" for a return address.
std::string describeFrame(void *address);

} // namespace mac_hsa
