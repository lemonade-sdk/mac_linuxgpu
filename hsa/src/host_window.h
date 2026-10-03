#pragma once

// The host window is the range of this process's address space where the
// driver places shared (GTT) buffers: each one is mapped at its GPU VA, so a
// CPU pointer and a GPU address are the same number.
//
// The driver picks those addresses inside the window, so nothing else in the
// process may occupy it. HostWindowReservation holds every free page of the
// window as an inaccessible reservation for the life of the session, and
// gives a range back only for the moment a shared buffer is mapped over it.
// Without it the process's own mappings (a model file's mmap, a large
// malloc) can land inside the window, and a later shared buffer whose GPU VA
// falls there cannot be mapped.

#include <cstdint>
#include <map>
#include <optional>

namespace mac_hsa {

class HostWindowReservation {
public:
    HostWindowReservation() = default;
    HostWindowReservation(const HostWindowReservation &) = delete;
    HostWindowReservation &operator=(const HostWindowReservation &) = delete;
    ~HostWindowReservation() { clear(); }

    // Reserves every free page of [base, base + bytes). Pages something else
    // already maps stay as they are. Returns the bytes now reserved.
    uint64_t reserve(uint64_t base, uint64_t bytes);
    // Releases [address, address + bytes) when this object reserved all of
    // it, so a placed mapping can take it. False leaves everything as it was:
    // the range is (partly) not ours, and a placed mapping there will collide
    // with whatever holds it, or find it free.
    bool take(uint64_t address, uint64_t bytes);
    // Reserves [address, address + bytes) again after a mapping there is
    // gone. Inside the window only; best effort (something may have mapped
    // the range in between, which then stays outside the reservation).
    void give(uint64_t address, uint64_t bytes);
    // Releases every reservation.
    void clear();

    uint64_t reservedBytes() const;
    bool contains(uint64_t address, uint64_t bytes) const;

private:
    bool reserveRange(uint64_t address, uint64_t bytes);
    void insert(uint64_t begin, uint64_t end);
    uint64_t windowBase = 0, windowEnd = 0;
    std::map<uint64_t, uint64_t> ranges; // begin -> end, disjoint, coalesced
};

// The ceiling on host memory the runtime may share with the GPU at once
// (every shared buffer: staging, kernel arguments, signals, AQL rings).
// Zero means no ceiling beyond the host window itself.
//
// Set from MAC_HSA_HOST_MEMORY_BUDGET (bytes, with an optional K, M or G
// suffix, powers of 1024) or by mac_hsa_set_host_memory_budget(); the call
// wins over the environment.
std::optional<uint64_t> parseHostMemoryBudget(const char *text);
void setHostMemoryBudget(uint64_t bytes);
uint64_t hostMemoryBudget();

// The window a client asks the driver for: the largest power of two that is
// no larger than the offered window and, when a budget is set, no larger
// than the budget rounded up to a power of two (so a 3 GiB budget asks for
// 4 GiB). Never below 16 KiB.
uint64_t requestedHostWindow(uint64_t offered, uint64_t budget);

} // namespace mac_hsa
