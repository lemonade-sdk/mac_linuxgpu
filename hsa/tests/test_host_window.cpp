// Host unit test: the host window reservation, the host memory budget and
// the window size a client asks the driver for (src/host_window.h).
//
// The reservation runs against this process's real address space: a window
// is placed, a foreign mapping is put inside it first, and the test checks
// that the reservation holds every other page, keeps the process's own
// mappings out of it, and hands ranges to placed mappings and back.

#include "host_window.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

namespace {
constexpr uint64_t MiB = 1ull << 20;
// A placed, non-overwriting mapping, as IOConnectMapMemory64 without
// kIOMapAnywhere makes one.
bool placedMap(uint64_t address, uint64_t bytes) {
    mach_vm_address_t placed = address;
    return mach_vm_allocate(mach_task_self(), &placed, bytes, VM_FLAGS_FIXED) == KERN_SUCCESS &&
        placed == address;
}
void unmap(uint64_t address, uint64_t bytes) { mach_vm_deallocate(mach_task_self(), address, bytes); }
// An aligned free range of the given size.
uint64_t freeRange(uint64_t bytes) {
    mach_vm_address_t address = 0;
    if (mach_vm_map(mach_task_self(), &address, bytes, bytes - 1, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0,
                    false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE) != KERN_SUCCESS) return 0;
    unmap(address, bytes);
    return address;
}
}

int main() {
    using namespace mac_hsa;
    const uint64_t window = 512 * MiB;
    const uint64_t base = freeRange(window);
    CHECK(base != 0, "found a free window");
    if (!base) return 1;

    // Something of the process's own already maps 4 MiB inside the window.
    const uint64_t foreign = base + 120 * MiB;
    CHECK(placedMap(foreign, 4 * MiB), "a foreign mapping inside the window");
    {
        HostWindowReservation reservation;
        CHECK(reservation.reserve(base, window) == window - 4 * MiB,
              "every free page of the window is reserved");
        CHECK(!reservation.contains(foreign, 4 * MiB), "the foreign mapping is left alone");
        CHECK(!placedMap(base + 32 * MiB, 32 * MiB), "a reserved range refuses a placed mapping");

        // The process's anywhere-mappings cannot land in the window.
        bool inside = false;
        for (int i = 0; i < 16; ++i) {
            void *p = mmap(reinterpret_cast<void *>(base + uint64_t(i) * 16 * MiB), 16 * MiB,
                           PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p == MAP_FAILED) continue;
            const auto address = reinterpret_cast<uint64_t>(p);
            if (address + 16 * MiB > base && address < base + window) inside = true;
            munmap(p, 16 * MiB);
        }
        CHECK(!inside, "hinted mmaps land outside the window");

        // Shared buffers of 16, 32, 64 and (below) 256 MiB, mapped at their GPU VA.
        uint64_t offset = 0;
        for (const uint64_t bytes : {16 * MiB, 32 * MiB, 64 * MiB}) {
            const uint64_t address = base + offset;
            CHECK(reservation.take(address, bytes), "take releases a held range");
            CHECK(placedMap(address, bytes), "a placed mapping lands in a taken range");
            offset += bytes;
        }
        CHECK(!reservation.take(foreign - 8 * MiB, 16 * MiB), "take refuses a range it does not hold");
        CHECK(reservation.contains(foreign - 8 * MiB, 8 * MiB), "a refused take changes nothing");
        CHECK(reservation.take(base + 256 * MiB, 256 * MiB), "a 256 MiB range up to the window end");
        CHECK(placedMap(base + 256 * MiB, 256 * MiB), "maps there");
        CHECK(reservation.reservedBytes() == window - 4 * MiB - 112 * MiB - 256 * MiB,
              "held bytes account for every mapped buffer");

        // Freeing a buffer gives its range back.
        unmap(base + 16 * MiB, 32 * MiB);
        reservation.give(base + 16 * MiB, 32 * MiB);
        CHECK(reservation.contains(base + 16 * MiB, 32 * MiB), "an unmapped range is held again");
        CHECK(!placedMap(base + 16 * MiB, 32 * MiB), "and refuses other mappings");
        reservation.give(base + window, 16 * MiB);
        CHECK(!reservation.contains(base + window, 16 * MiB), "give ignores ranges outside the window");
        CHECK(reservation.take(base + 16 * MiB, 32 * MiB) && placedMap(base + 16 * MiB, 32 * MiB),
              "a range given back can be taken again");

        for (const auto &[address, bytes] : {std::pair{base, 16 * MiB}, {base + 16 * MiB, 32 * MiB},
                                             {base + 48 * MiB, 64 * MiB}, {base + 256 * MiB, 256 * MiB}})
            unmap(address, bytes);
    }
    unmap(foreign, 4 * MiB);
    CHECK(placedMap(base, window), "the destructor releases the whole reservation");
    unmap(base, window);

    // The budget setting.
    CHECK(!parseHostMemoryBudget(nullptr) && !parseHostMemoryBudget(""), "unset budget");
    CHECK(parseHostMemoryBudget("4096") == 4096u, "bytes");
    CHECK(parseHostMemoryBudget("4G") == (4ull << 30) && parseHostMemoryBudget("4GiB") == (4ull << 30) &&
          parseHostMemoryBudget("512m") == (512ull << 20) && parseHostMemoryBudget("64K") == (64ull << 10),
          "K/M/G suffixes");
    CHECK(!parseHostMemoryBudget("4X") && !parseHostMemoryBudget("-1") && !parseHostMemoryBudget("G") &&
          !parseHostMemoryBudget("99999999999999999999") && !parseHostMemoryBudget("17179869184G"),
          "malformed budgets are ignored");
    setHostMemoryBudget(3ull << 30);
    CHECK(hostMemoryBudget() == (3ull << 30), "the API sets the budget");
    setHostMemoryBudget(0);
    CHECK(hostMemoryBudget() == 0, "zero removes it");

    // The window asked for.
    CHECK(requestedHostWindow(16ull << 30, 0) == (16ull << 30), "no budget: the offered window");
    CHECK(requestedHostWindow(16ull << 30, 4ull << 30) == (4ull << 30), "a 4 GiB budget asks for 4 GiB");
    CHECK(requestedHostWindow(16ull << 30, 3ull << 30) == (4ull << 30), "a budget rounds up to a power of two");
    CHECK(requestedHostWindow(2ull << 30, 4ull << 30) == (2ull << 30), "never more than offered");
    CHECK(requestedHostWindow(16ull << 30, 1) == 16384, "never below 16 KiB");

    std::fprintf(stderr, failures ? "%d FAILURE(S)\n" : "all host window checks passed\n", failures);
    return failures ? 1 : 0;
}
