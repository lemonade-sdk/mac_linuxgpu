/* Exercise the actual failure/close path. Every reachable OS call is replaced
 * below; this process never opens a real service or reserves client memory. */
#define MAC_HSA_IDLE_DIAGNOSTIC
#define IOServiceOpen test_service_open
#define IOServiceClose test_service_close
#define IOConnectCallScalarMethod test_scalar
#define mach_vm_map test_vm_map
#define mach_vm_deallocate test_vm_deallocate
#define mlg_fw_service_start_connection test_fw_start
#define mlg_fw_service_stop test_fw_stop
/* The IOKit calls are replaced: every selector reaches them synchronously. */
#define MLG_SELECTOR_CALL_TEST_SYNC
#include "../src/transport_iokit.cpp"
#include <cassert>

static unsigned opens, closes, reserves, releases, init_calls, probes, logs, queries;
static unsigned fw_starts, fw_stops;
static bool fw_running;
static struct mlg_fw_service *const fw_token = reinterpret_cast<struct mlg_fw_service *>(0x5eed);
static bool port_open;
static constexpr io_connect_t test_port = 777;
/* The driver's GART aperture size; the runtime reserves what it reports. */
static constexpr uint64_t window_bytes = 1ull << 30;
static constexpr uint64_t window_base = 0x400000000ull;

extern "C" kern_return_t test_service_open(io_service_t service, task_port_t,
                                           uint32_t type, io_connect_t *port) {
    assert(service == 42 && !type && !port_open && !opens++);
    *port = test_port; port_open = true; return KERN_SUCCESS;
}
/* The firmware servicer runs on the owner connection exactly while
 * InitDevice (selector 9) is in flight, and stops although it fails. */
extern "C" int test_fw_start(uint32_t connection, const char *root, struct mlg_fw_service **out) {
    assert(connection == test_port && port_open && !root && out && !*out);
    assert(reserves == 1 && !init_calls && !fw_running && !fw_starts++);
    fw_running = true; *out = fw_token; return 0;
}
extern "C" void test_fw_stop(struct mlg_fw_service *service) {
    if (!service) return; /* Destructor's defensive stop of an idle servicer. */
    assert(service == fw_token && fw_running && init_calls == 1 && !releases && !fw_stops++);
    fw_running = false;
}
extern "C" kern_return_t test_service_close(io_connect_t port) {
    assert(port == test_port && port_open && probes == 1 && logs == 1);
    assert(fw_starts == 1 && fw_stops == 1 && !fw_running);
    assert(init_calls == 1 && releases == 1 && !closes++);
    port_open = false; return KERN_SUCCESS;
}
extern "C" kern_return_t test_vm_map(vm_map_t, mach_vm_address_t *address,
    mach_vm_size_t size, mach_vm_offset_t, int, mem_entry_name_port_t,
    memory_object_offset_t, boolean_t, vm_prot_t, vm_prot_t, vm_inherit_t) {
    assert(port_open && queries == 1 && size == window_bytes && !reserves++);
    *address = window_base; return KERN_SUCCESS;
}
extern "C" kern_return_t test_vm_deallocate(vm_map_t, mach_vm_address_t address, mach_vm_size_t size) {
    assert(port_open && init_calls == 1 && address == window_base && size == window_bytes);
    releases++; return KERN_SUCCESS;
}
extern "C" kern_return_t test_scalar(mach_port_t port, uint32_t selector,
    const uint64_t *in, uint32_t inputs, uint64_t *out, uint32_t *count) {
    assert(port == test_port && port_open && !closes);
    if (selector != 9) assert(!fw_running);
    if (init_calls) {
        /* Once init fails, diagnostics are exclusively the two cached tags. */
        assert(selector == 21);
        if (inputs == 1 && in[0] == mac_hsa::kCachedProbeStatus) {
            assert(*count == 5 && !probes++);
            out[0] = 1; out[1] = 0; out[2] = uint64_t(-38ll); out[3] = 2; out[4] = 4;
            return KERN_SUCCESS;
        }
        assert(inputs == 2 && in[0] == mac_hsa::kCachedKernelLog && !in[1] && !logs++);
        assert(*count == 16);
        const char message[] = "Failed initializing VRAM heap\n";
        constexpr size_t bytes = sizeof(message) - 1;
        out[0] = out[1] = bytes; out[2] = bytes;
        std::memcpy(out + 3, message, bytes);
        *count = 3 + (bytes + 7) / 8;
        return KERN_SUCCESS;
    }
    switch (selector) {
    case 43: assert(!inputs && *count == 3); out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 219; break;
    case 1: assert(!inputs && *count == 7); out[3] = 0x1002; out[4] = 0x1234; out[6] = 0x01; break;
    case 21: assert(inputs == 1 && in[0] == 4 && *count == 1); out[0] = 0; break;
    case 54: assert(inputs == 1 && *count == 3);
        if (!in[0]) { assert(!queries++ && !reserves); out[0] = 0; }
        else { assert(in[0] == window_base && queries == 1 && reserves == 1); out[0] = in[0]; }
        out[1] = window_bytes; out[2] = 0; break;
    case 9: assert(!inputs && !*count && fw_running); init_calls++; return kIOReturnError;
    default: assert(false);
    }
    return KERN_SUCCESS;
}

namespace mac_hsa { namespace {
struct IdleDiagnosticAccess {
    static void run() {
        IOKitConnection connection(OriginalAtomicCaps{}, true);
        connection.service = 42;
        assert(connection.ensureReady() == HSA_STATUS_ERROR);
        assert(connection.state == IOKitConnection::State::Faulted && !connection.ownerPort);
        assert(connection.initializationTransportFailure.present);
        assert(connection.initializationTransportFailure.selector == 9);
        assert(connection.initializationTransportFailure.result == uint32_t(kIOReturnError));
        assert(connection.ensureReady() == HSA_STATUS_ERROR); /* No retry/open. */
        assert(opens == 1 && closes == 1 && probes == 1 && logs == 1);
        assert(fw_starts == 1 && fw_stops == 1 && !fw_running);
        connection.service = 0; /* The synthetic service has no OS reference. */
    }
};
} }

int main() {
    mac_hsa::IdleDiagnosticAccess::run();
    std::puts("Production HSA failure diagnostics used the existing owner before close; no retries");
    std::puts("Production HSA ran the firmware servicer around the failing InitDevice and stopped it");
}
