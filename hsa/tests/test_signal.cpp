// Host unit test: the HSA signal state machine.
//
// Exercises hsa_signal_create / hsa_signal_store_* / hsa_signal_wait_* /
// hsa_signal_load_* / the atomic RMW family (add/subtract/and/or/xor/exchange/
// cas) against the fake backend. The fake simulates the GPU signal-op kernel
// (its dispatchAQL performs the atomic on the shared arena), so the full
// GPU-mediated signal path is exercised end-to-end on the host.

#include "mac_hsa.h"
#include "transport_fake.h"
#include <hsa/hsa_ext_amd.h>
#include <hsa/amd_hsa_signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", msg); } \
} while (0)

int main() {
    hsa_status_t status = hsa_init();
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_init");
    if (status != HSA_STATUS_SUCCESS) return 1;

    // Create a signal (GPU-backed, since the fake reports the GPU-mediating
    // capability). The fake's dispatchAQL simulates the signal-op atomic.
    hsa_signal_t sig{};
    status = hsa_signal_create(0, 0, nullptr, &sig);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_signal_create");
    if (status != HSA_STATUS_SUCCESS) { hsa_shut_down(); return failures ? 1 : 0; }

    // 1. Initial value is 0.
    CHECK(hsa_signal_load_relaxed(sig) == 0, "initial value is 0");

    // 2. Store relaxed -> load sees the value.
    hsa_signal_store_relaxed(sig, 42);
    CHECK(hsa_signal_load_relaxed(sig) == 42, "store_relaxed(42) -> load==42");

    // 3. Store screlease -> load_scacquire sees the value.
    hsa_signal_store_screlease(sig, 100);
    CHECK(hsa_signal_load_scacquire(sig) == 100, "store_screlease(100) -> load_scacquire==100");

    // 4. add relaxed: 100 + 23 = 123.
    hsa_signal_add_relaxed(sig, 23);
    CHECK(hsa_signal_load_relaxed(sig) == 123, "add_relaxed(23): 100+23==123");

    // 5. subtract relaxed: 123 - 23 = 100.
    hsa_signal_subtract_relaxed(sig, 23);
    CHECK(hsa_signal_load_relaxed(sig) == 100, "subtract_relaxed(23): 123-23==100");

    // 6. and relaxed: 100 & 0xFF = 100.
    hsa_signal_and_relaxed(sig, 0xFF);
    CHECK(hsa_signal_load_relaxed(sig) == 100, "and_relaxed(0xFF): 100&0xFF==100");

    // 7. or relaxed: 100 | 0x100 = 356.
    hsa_signal_or_relaxed(sig, 0x100);
    CHECK(hsa_signal_load_relaxed(sig) == 356, "or_relaxed(0x100): 100|0x100==356");

    // 8. xor relaxed: 356 ^ 0x100 = 100.
    hsa_signal_xor_relaxed(sig, 0x100);
    CHECK(hsa_signal_load_relaxed(sig) == 100, "xor_relaxed(0x100): 356^0x100==100");

    // 9. exchange relaxed: old=100, new=7.
    const auto old = hsa_signal_exchange_relaxed(sig, 7);
    CHECK(old == 100, "exchange_relaxed(7): old==100");
    CHECK(hsa_signal_load_relaxed(sig) == 7, "exchange_relaxed(7): new==7");

    // 10. cas relaxed (success): expected=7, desired=99 -> old=7, new=99.
    int64_t expected = 7;
    const auto casOld = hsa_signal_cas_relaxed(sig, expected, 99);
    CHECK(casOld == 7, "cas_relaxed(7->99): returned old==7");
    CHECK(hsa_signal_load_relaxed(sig) == 99, "cas_relaxed(7->99): new==99");

    // 11. cas relaxed (failure): expected=7 (but value is 99) -> fails, value unchanged.
    expected = 7;
    hsa_signal_cas_relaxed(sig, expected, 55);
    CHECK(hsa_signal_load_relaxed(sig) == 99, "cas_relaxed(fail): value unchanged (99)");

    // 12. wait_relaxed GTE: wait until value >= 99 (it already is).
    const auto waited = hsa_signal_wait_relaxed(sig, HSA_SIGNAL_CONDITION_GTE, 99, 1000000, HSA_WAIT_STATE_BLOCKED);
    CHECK(waited == 99, "wait_relaxed(GTE 99): returns 99");

    // 13. wait_relaxed EQ: store 0, wait until value == 0.
    hsa_signal_store_relaxed(sig, 0);
    const auto waited0 = hsa_signal_wait_relaxed(sig, HSA_SIGNAL_CONDITION_EQ, 0, 1000000, HSA_WAIT_STATE_BLOCKED);
    CHECK(waited0 == 0, "wait_relaxed(EQ 0): returns 0");

    // 14. wait_all / wait_any (the AMD variants).
    hsa_signal_t sigs[2];
    status = hsa_signal_create(0, 0, nullptr, &sigs[0]);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_signal_create (sig2)");
    hsa_signal_store_relaxed(sigs[0], 5);
    hsa_signal_store_relaxed(sig, 0); // sig is already 0
    hsa_signal_condition_t conds[2] = {HSA_SIGNAL_CONDITION_EQ, HSA_SIGNAL_CONDITION_EQ};
    hsa_signal_value_t vals[2] = {5, 0};
    const uint32_t r = hsa_amd_signal_wait_all(2, sigs, conds, vals, 1000000, HSA_WAIT_STATE_BLOCKED, nullptr);
    CHECK(r == 0, "wait_all: both satisfied (returns 0)");

    // 15. Silent store (no notify) -> load still sees the value.
    hsa_signal_silent_store_relaxed(sig, 3);
    CHECK(hsa_signal_load_relaxed(sig) == 3, "silent_store_relaxed(3) -> load==3");

    // 16. Destroy.
    status = hsa_signal_destroy(sig);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_signal_destroy");
    status = hsa_signal_destroy(sigs[0]);
    CHECK(status == HSA_STATUS_SUCCESS, "hsa_signal_destroy (sig2)");

    hsa_shut_down();
    std::fprintf(stderr, "\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
