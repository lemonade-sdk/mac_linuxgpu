/* DriverKit substitutes for compiling dext/sources/dext_main.mm whole
 * (test_pci_call_guards.cpp). The IOPCIDevice records every call and checks
 * it against what the kernel guarantees safe: a session open for the
 * caller, a memory index of a BAR, an access inside that BAR, a config
 * offset inside the 4 KiB space, and no Close while a call is in flight. A
 * call outside that contract is a violation, which the test requires none
 * of; the guards must stop such a call before it is made. */
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <functional>

using kern_return_t = int;
using IOOptionBits = uint32_t;
constexpr kern_return_t kIOReturnSuccess = 0;
constexpr kern_return_t kIOReturnNotFound = (int)0xe00002f0;
constexpr kern_return_t kIOReturnNotOpen = (int)0xe00002cd;
constexpr kern_return_t kIOReturnBadArgument = (int)0xe00002c2;
constexpr uint8_t kPCIBARTypeIO = 0x00;
constexpr uint8_t kPCIBARTypeM32 = 0x01;
constexpr uint8_t kPCIBARTypeM64 = 0x04;
constexpr uint32_t kIOPCIDeviceResetTypeFunctionReset = 1;
constexpr uint32_t kIOPCIDeviceResetOptionNone = 0;
constexpr uint32_t kIOPCICapabilityIDMSIX = 0x11;
constexpr uint32_t kIOInterruptTypePCIMessaged = 0x10000;
constexpr uint32_t kIOInterruptTypePCIMessagedX = 0x20000;

#define IOLog(...) fprintf(stderr, __VA_ARGS__)
static void IOSleep(unsigned ms) { usleep(ms * 1000); }

class OSAction {};
class IODispatchQueue { public: void release() {} };
class IOService {
public:
    kern_return_t CopyDispatchQueue(const char *, IODispatchQueue **queue) {
        static IODispatchQueue q; *queue = &q; return kIOReturnSuccess;
    }
};
class IOMemoryDescriptor {
public:
    uint64_t length = 0;
    kern_return_t GetLength(uint64_t *out) { *out = length; return kIOReturnSuccess; }
    void release() { delete this; }
};
class IOInterruptDispatchSource {
public:
    static kern_return_t Create(IOService *, uint32_t, IODispatchQueue *, IOInterruptDispatchSource **out) {
        *out = new IOInterruptDispatchSource; return kIOReturnSuccess;
    }
    kern_return_t SetHandler(OSAction *) { return kIOReturnSuccess; }
    kern_return_t SetEnable(bool) { return kIOReturnSuccess; }
    kern_return_t Cancel(void (^done)(void)) { done(); return kIOReturnSuccess; }
    kern_return_t SetEnableWithCompletion(bool, void (^done)(void)) { done(); return kIOReturnSuccess; }
    void release() { delete this; }
};

/* The R9700's device memory as IOPCIFamily lists it: BAR0 (256 MiB, 64-bit),
 * BAR2 (2 MiB, 64-bit), BAR5 (512 KiB) and the expansion ROM (128 KiB, not
 * a BAR: GetBARInfo never names it). */
struct mock_memory { uint64_t size; uint8_t bar; bool is_bar; };
static const mock_memory k_mock_memory[] = {
    { 256ull << 20, 0, true }, { 2ull << 20, 2, true }, { 512ull << 10, 5, true },
    { 128ull << 10, 6, false },
};

class IOPCIDevice : public IOService {
public:
    std::atomic<bool> open{false};
    IOService *owner = nullptr;
    std::atomic<unsigned> calls{0}, memory_calls{0}, config_calls{0}, closes{0}, in_flight{0};
    std::atomic<unsigned> violations{0};
    /* Runs inside the next MemoryRead32/ConfigurationRead32, mid-call. */
    std::function<void()> during_read32;
    /* Runs inside Close, after its checks, before the session closes
     * (async-signal-safe: the crash close calls it from a handler). */
    void (*during_close)(void) = nullptr;

    void violation(const char *what, uint64_t a, uint64_t b) {
        fprintf(stderr, "kernel contract violation: %s (%#llx, %#llx)\n", what,
                (unsigned long long)a, (unsigned long long)b);
        ++violations;
    }
    struct call {
        IOPCIDevice &d;
        explicit call(IOPCIDevice &device) : d(device) { ++d.calls; ++d.in_flight; }
        ~call() { --d.in_flight; }
    };
    void memory(uint8_t index, uint64_t offset, unsigned width) {
        ++memory_calls;
        if (!open) violation("memory access on a closed session", index, offset);
        if (index >= sizeof(k_mock_memory) / sizeof(k_mock_memory[0]) || !k_mock_memory[index].is_bar)
            violation("memory index of no BAR", index, offset);
        else if (offset >= k_mock_memory[index].size || width > k_mock_memory[index].size - offset)
            violation("memory access outside its BAR", index, offset);
    }
    void config(uint64_t offset, unsigned width) {
        ++config_calls;
        if (!open) violation("config access on a closed session", offset, width);
        if (offset >= 4096 || width > 4096 - offset || (offset & (width - 1)))
            violation("config access outside the config space", offset, width);
    }

    kern_return_t Open(IOService *client, IOOptionBits) {
        call c(*this);
        if (open) return kIOReturnNotOpen;
        open = true; owner = client; return kIOReturnSuccess;
    }
    kern_return_t Close(IOService *client, IOOptionBits) {
        call c(*this);
        ++closes;
        if (in_flight != 1) violation("Close with a call in flight", in_flight - 1, 0);
        if (!open || client != owner) violation("Close of a session not open", open, 0);
        if (during_close) during_close();
        open = false; owner = nullptr;
        return kIOReturnSuccess;
    }
    kern_return_t GetBARInfo(uint8_t bar, uint8_t *index, uint64_t *size, uint8_t *type) {
        call c(*this);
        for (uint8_t i = 0; i < sizeof(k_mock_memory) / sizeof(k_mock_memory[0]); i++)
            if (k_mock_memory[i].is_bar && k_mock_memory[i].bar == bar) {
                *index = i; *size = k_mock_memory[i].size;
                *type = bar == 5 ? kPCIBARTypeM32 : kPCIBARTypeM64;
                return kIOReturnSuccess;
            }
        return kIOReturnNotFound;
    }
    kern_return_t _CopyDeviceMemoryWithIndex(uint64_t index, IOMemoryDescriptor **out, IOService *) {
        call c(*this);
        if (!open) violation("CopyDeviceMemory on a closed session", index, 0);
        if (index >= sizeof(k_mock_memory) / sizeof(k_mock_memory[0])) return kIOReturnBadArgument;
        *out = new IOMemoryDescriptor; (*out)->length = k_mock_memory[index].size;
        return kIOReturnSuccess;
    }
    void MemoryRead8(uint8_t i, uint64_t o, uint8_t *v) { call c(*this); memory(i, o, 1); *v = 0x5a; }
    void MemoryRead16(uint8_t i, uint64_t o, uint16_t *v) { call c(*this); memory(i, o, 2); *v = 0x5a5a; }
    void MemoryRead32(uint8_t i, uint64_t o, uint32_t *v) {
        call c(*this); memory(i, o, 4); *v = 0x5a5a5a5a;
        if (auto f = std::move(during_read32)) { during_read32 = nullptr; f(); }
    }
    void MemoryRead64(uint8_t i, uint64_t o, uint64_t *v) { call c(*this); memory(i, o, 8); *v = 0x5a5a5a5a5a5a5a5aull; }
    void MemoryWrite8(uint8_t i, uint64_t o, uint8_t) { call c(*this); memory(i, o, 1); }
    void MemoryWrite16(uint8_t i, uint64_t o, uint16_t) { call c(*this); memory(i, o, 2); }
    void MemoryWrite32(uint8_t i, uint64_t o, uint32_t) { call c(*this); memory(i, o, 4); }
    void MemoryWrite64(uint8_t i, uint64_t o, uint64_t) { call c(*this); memory(i, o, 8); }
    void ConfigurationRead8(uint64_t o, uint8_t *v) { call c(*this); config(o, 1); *v = 0; }
    void ConfigurationRead16(uint64_t o, uint16_t *v) { call c(*this); config(o, 2); *v = 0; }
    void ConfigurationRead32(uint64_t o, uint32_t *v) {
        call c(*this); config(o, 4); *v = o == 0 ? 0x75511002u : 0;
        if (auto f = std::move(during_read32)) { during_read32 = nullptr; f(); }
    }
    void ConfigurationWrite8(uint64_t o, uint8_t) { call c(*this); config(o, 1); }
    void ConfigurationWrite16(uint64_t o, uint16_t) { call c(*this); config(o, 2); }
    void ConfigurationWrite32(uint64_t o, uint32_t) { call c(*this); config(o, 4); }
    kern_return_t GetBusDeviceFunction(uint8_t *b, uint8_t *d, uint8_t *f) {
        call c(*this); *b = 5; *d = 0; *f = 0; return kIOReturnSuccess;
    }
    kern_return_t Reset(uint32_t, uint32_t) { call c(*this); return kIOReturnSuccess; }
    kern_return_t FindPCICapability(uint32_t, uint64_t, uint64_t *found) {
        call c(*this); *found = 0xa0; return kIOReturnSuccess;
    }
    kern_return_t ConfigureInterrupts(uint32_t, uint32_t, uint32_t, uint32_t) {
        call c(*this); return kIOReturnSuccess;
    }
    void retain() {}
    void release() {}
};
