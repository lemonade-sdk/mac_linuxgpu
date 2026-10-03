/* The exact production FLR sequence runs against byte-addressed mock config. */
#include <cassert>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include "../../dext/sources/pci_reset_policy.h"
#include "../../dext/sources/pci_access_gate.h"
struct PCI {
    uint8_t config[4096]{};
    unsigned writes = 0, resets = 0, sleeps = 0;
    bool suppress_command = false, fail_reset = false, change_bar = false;
    void ConfigurationRead8(unsigned o, uint8_t *v) { assert(o < sizeof(config)); *v = config[o]; }
    void ConfigurationRead16(unsigned o, uint16_t *v) { assert(o+2 <= sizeof(config)); memcpy(v, config+o, 2); }
    void ConfigurationRead32(unsigned o, uint32_t *v) { assert(o+4 <= sizeof(config)); memcpy(v, config+o, 4); }
    void ConfigurationWrite16(unsigned o, uint16_t v) {
        assert(o == 4); ++writes;
        if (!suppress_command) memcpy(config+o, &v, 2);
    }
    void set16(unsigned o, uint16_t v) { memcpy(config+o, &v, 2); }
    void set32(unsigned o, uint32_t v) { memcpy(config+o, &v, 4); }
    PCI() {
        set32(0, 0x744c1002); set16(4, 6); set16(6, 0x10);
        config[0x34] = 0x80; config[0x80] = 0x10;
        set32(0x84, 1u << 28); set32(0x10, 0x8000000c);
    }
    int run(bool &changed, bool shutdown = false) {
        return dext_reset_endpoint(*this, [&] {
            ++resets;
            uint16_t cmd; ConfigurationRead16(4, &cmd); assert(!(cmd & 4));
            if (change_bar) set32(0x10, 0x9000000c);
            return fail_reset ? -1 : 0;
        }, [&] { ++sleeps; }, changed, !shutdown);
    }
};
int main() {
    bool changed;
    PCI good; assert(good.run(changed) == 0 && changed);
    assert(good.resets == 1 && good.writes == 2 && good.sleeps == 0);
    uint16_t command; good.ConfigurationRead16(4, &command); assert(command == 6);
    PCI shutdown; assert(shutdown.run(changed, true) == 0 && changed);
    assert(shutdown.resets == 1 && shutdown.writes == 1);
    shutdown.ConfigurationRead16(4, &command); assert(command == 2);
    PCI unsupported; unsupported.set32(0x84, 0);
    assert(unsupported.run(changed) == -95 && !changed && !unsupported.writes);
    PCI cycle; cycle.config[0x81] = 0x80;
    assert(cycle.run(changed) < 0 && !changed && !cycle.resets);
    PCI disappeared; disappeared.set32(0, UINT32_MAX);
    assert(disappeared.run(changed) == -19 && !changed && !disappeared.writes);
    PCI suppress; suppress.suppress_command = true;
    assert(suppress.run(changed) < 0 && changed && !suppress.resets);
    PCI pending; pending.set16(0x8a, 1 << 5);
    assert(pending.run(changed) == -110 && changed && !pending.resets && pending.sleeps == 1000);
    PCI failure; failure.fail_reset = true;
    assert(failure.run(changed) < 0 && failure.resets == 1 && failure.writes == 1);
    PCI moved; moved.change_bar = true;
    assert(moved.run(changed) < 0 && moved.resets == 1 && moved.writes == 1);
    PCI isolated; isolated.set32(0x84, 0); /* isolation needs no FLR support */
    assert(!dext_isolate_endpoint(isolated, [] {}));
    isolated.ConfigurationRead16(4, &command); assert(command == 2);
    assert(isolated.resets == 0 && isolated.writes == 1);
    PCI isolation_failed; isolation_failed.suppress_command = true;
    assert(dext_isolate_endpoint(isolation_failed, [] {}) < 0);
    PCI isolation_pending; isolation_pending.set16(0x8a, 1 << 5);
    assert(dext_isolate_endpoint(isolation_pending, [&] { ++isolation_pending.sleeps; }) == -110);
    assert(isolation_pending.sleeps == 1000 && isolation_pending.resets == 0);
    PCI isolation_bad_cap; isolation_bad_cap.config[0x81] = 0x80;
    assert(dext_isolate_endpoint(isolation_bad_cap, [] {}) < 0);
    isolation_bad_cap.ConfigurationRead16(4, &command); assert(command == 2);
    dext_pci_access_gate gate;
    std::atomic<bool> entered{false}, finish{false};
    std::thread rpc([&] {
        assert(gate.enter()); entered = true;
        while (!finish) std::this_thread::yield();
        gate.leave();
    });
    while (!entered) std::this_thread::yield();
    gate.block();
    assert(gate.closed() && !gate.drained() && !gate.enter());
    finish = true; rpc.join();
    assert(gate.drained() && !gate.enter());
    return 0;
}
