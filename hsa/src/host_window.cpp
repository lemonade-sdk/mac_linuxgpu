#include "host_window.h"
#include "mach_vm_compat.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <cerrno>
#include <cstdlib>

namespace mac_hsa {
namespace {
uint64_t pageSize() { return uint64_t(vm_page_size); }

// The next mapped region at or above address: [begin, end), or nothing.
bool nextRegion(uint64_t address, uint64_t &begin, uint64_t &end) {
    mach_vm_address_t found = address;
    mach_vm_size_t bytes = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &found, &bytes, VM_REGION_BASIC_INFO_64,
                                       reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (result != KERN_SUCCESS) return false;
    begin = found; end = found + bytes;
    return true;
}
} // namespace

bool HostWindowReservation::reserveRange(uint64_t address, uint64_t bytes) {
    mach_vm_address_t placed = address;
    const auto result = mach_vm_map(mach_task_self(), &placed, bytes, 0, VM_FLAGS_FIXED, MACH_PORT_NULL,
                                    0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) return false;
    if (placed != address) { (void)mach_vm_deallocate(mach_task_self(), placed, bytes); return false; }
    insert(address, address + bytes);
    return true;
}

void HostWindowReservation::insert(uint64_t begin, uint64_t end) {
    auto next = ranges.lower_bound(begin);
    if (next != ranges.begin()) {
        auto previous = std::prev(next);
        if (previous->second == begin) { begin = previous->first; ranges.erase(previous); }
    }
    next = ranges.lower_bound(begin);
    if (next != ranges.end() && next->first == end) { end = next->second; ranges.erase(next); }
    ranges.emplace(begin, end);
}

uint64_t HostWindowReservation::reserve(uint64_t base, uint64_t bytes) {
    clear();
    const auto page = pageSize();
    if (!bytes || (base | bytes) & (page - 1) || bytes > UINT64_MAX - base) return 0;
    windowBase = base; windowEnd = base + bytes;
    for (uint64_t address = base; address < windowEnd;) {
        uint64_t begin = 0, end = 0;
        const bool mapped = nextRegion(address, begin, end) && begin < windowEnd;
        const uint64_t gapEnd = mapped ? std::max(begin, address) : windowEnd;
        if (gapEnd > address) (void)reserveRange(address, gapEnd - address);
        if (!mapped) break;
        address = std::max(end, address + (gapEnd > address ? 0 : page));
    }
    return reservedBytes();
}

bool HostWindowReservation::take(uint64_t address, uint64_t bytes) {
    if (!bytes || bytes > UINT64_MAX - address) return false;
    const uint64_t end = address + bytes;
    auto found = ranges.upper_bound(address);
    if (found == ranges.begin()) return false;
    --found;
    const auto [begin, rangeEnd] = *found;
    if (address < begin || end > rangeEnd) return false;
    if (mach_vm_deallocate(mach_task_self(), address, bytes) != KERN_SUCCESS) return false;
    ranges.erase(found);
    if (begin < address) ranges.emplace(begin, address);
    if (end < rangeEnd) ranges.emplace(end, rangeEnd);
    return true;
}

void HostWindowReservation::give(uint64_t address, uint64_t bytes) {
    if (!bytes || bytes > UINT64_MAX - address || address < windowBase || address + bytes > windowEnd) return;
    if (contains(address, bytes)) return;
    (void)reserveRange(address, bytes);
}

void HostWindowReservation::clear() {
    for (const auto &[begin, end] : ranges) (void)mach_vm_deallocate(mach_task_self(), begin, end - begin);
    ranges.clear();
    windowBase = windowEnd = 0;
}

uint64_t HostWindowReservation::reservedBytes() const {
    uint64_t total = 0;
    for (const auto &[begin, end] : ranges) total += end - begin;
    return total;
}

bool HostWindowReservation::contains(uint64_t address, uint64_t bytes) const {
    auto found = ranges.upper_bound(address);
    if (found == ranges.begin()) return false;
    --found;
    return address >= found->first && address < found->second && bytes <= found->second - address;
}

std::optional<uint64_t> parseHostMemoryBudget(const char *text) {
    if (!text || !*text) return {};
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno || end == text || text[0] == '-') return {};
    unsigned shift = 0;
    switch (*end) {
    case '\0': break;
    case 'k': case 'K': shift = 10; ++end; break;
    case 'm': case 'M': shift = 20; ++end; break;
    case 'g': case 'G': shift = 30; ++end; break;
    default: return {};
    }
    if (*end == 'i' || *end == 'B') { if (*end == 'i') ++end; if (*end == 'B') ++end; }
    if (*end) return {};
    if (shift && value > (UINT64_MAX >> shift)) return {};
    return uint64_t(value) << shift;
}

namespace {
// UINT64_MAX: not set by the API; the environment decides.
std::atomic<uint64_t> configuredBudget{UINT64_MAX};
}

void setHostMemoryBudget(uint64_t bytes) { configuredBudget.store(bytes, std::memory_order_relaxed); }

uint64_t hostMemoryBudget() {
    const auto configured = configuredBudget.load(std::memory_order_relaxed);
    if (configured != UINT64_MAX) return configured;
    return parseHostMemoryBudget(std::getenv("MAC_HSA_HOST_MEMORY_BUDGET")).value_or(0);
}

uint64_t requestedHostWindow(uint64_t offered, uint64_t budget) {
    constexpr uint64_t minimum = 16384;
    if (offered < minimum || (offered & (offered - 1))) return offered;
    if (!budget) return offered;
    uint64_t want = minimum;
    while (want < budget && want < offered) want <<= 1;
    return want;
}

} // namespace mac_hsa
