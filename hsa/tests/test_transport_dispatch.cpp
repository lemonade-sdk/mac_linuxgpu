/* Selector 51 (ComputeDispatch) on the production IOKit transport. The
 * Linux-shim driver runs it as a one-shot AQL dispatch that borrows a queue
 * slot, and answers kIOReturnNoResources, before submitting anything, when
 * every slot is held. Like selector 55 that is "nothing submitted", not a
 * session fault. Only the IOKit calls are replaced; no service is opened. */
#define MAC_HSA_IDLE_DIAGNOSTIC
#define IOServiceClose test_service_close
#define IOConnectCallScalarMethod test_scalar
#define IOConnectCallMethod test_method
#define mlg_fw_service_start_connection test_fw_start
#define mlg_fw_service_stop test_fw_stop
/* The IOKit calls are replaced: every selector reaches them synchronously. */
#define MLG_SELECTOR_CALL_TEST_SYNC
#include "../src/transport_iokit.cpp"
#include <cassert>

/* No initialization runs here, so the firmware servicer never starts. */
extern "C" int test_fw_start(uint32_t, const char *, struct mlg_fw_service **) { assert(false); return -1; }
extern "C" void test_fw_stop(struct mlg_fw_service *service) { assert(!service); }

static constexpr io_connect_t test_port = 515;
static kern_return_t next_result = KERN_SUCCESS;
static unsigned dispatches, closes;
static uint64_t next_fence = 1;

extern "C" kern_return_t test_service_close(io_connect_t port) {
    assert(port == test_port); ++closes; return KERN_SUCCESS;
}
extern "C" kern_return_t test_scalar(mach_port_t port, uint32_t selector, const uint64_t *,
    uint32_t inputs, uint64_t *out, uint32_t *count) {
    assert(port == test_port && selector == 43 && !inputs && *count == 3);
    out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 219;
    return KERN_SUCCESS;
}
extern "C" kern_return_t test_method(mach_port_t port, uint32_t selector, const uint64_t *, uint32_t,
    const void *input, size_t inputBytes, uint64_t *out, uint32_t *count, void *, size_t *) {
    assert(port == test_port && selector == 51 && input && inputBytes == sizeof(amdgpu::ComputeDispatchRequest));
    assert(*count == 3);
    ++dispatches;
    if (next_result != KERN_SUCCESS) return next_result;
    out[0] = 0; out[1] = next_fence++; out[2] = 3;
    return KERN_SUCCESS;
}

namespace mac_hsa { namespace {
struct IdleDiagnosticAccess {
    static amdgpu::ComputeDispatchRequest request() {
        amdgpu::ComputeDispatchRequest r{};
        r.version = 2; r.codeHandle = 0x10000; r.codeBytes = sizeof(uint32_t);
        r.groups[0] = r.groups[1] = r.groups[2] = 1;
        r.threads[0] = 32; r.threads[1] = r.threads[2] = 1;
        r.rsrc1 = 0xc0000; r.timeoutUS = 100000;
        return r;
    }
    static void run() {
        IOKitConnection connection(OriginalAtomicCaps{}, true);
        connection.ownerPort = test_port;
        connection.state = IOKitConnection::State::Ready;
        connection.queueSlotLimit = 1; /* one AQL queue slot, as on the R9700 */
        uint64_t fence = 0;

        /* Another client holds the slot: refused, nothing submitted, healthy. */
        next_result = kIOReturnNoResources;
        assert(connection.dispatch(request(), fence) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
        assert(dispatches == 1 && !fence && connection.state == IOKitConnection::State::Ready);
        next_result = KERN_SUCCESS;
        assert(connection.dispatch(request(), fence) == HSA_STATUS_SUCCESS && fence == 1);

        /* This session's persistent queue holds the only slot: refused
         * without asking the driver. The loader's code-sync takes the same
         * path and leaves the session healthy. */
        connection.hardwareQueues.emplace(7, std::array<uint64_t, 2>{1, 2});
        assert(connection.dispatch(request(), fence) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
        connection.codeSyncBuffer = {0x10000, 0x200000000ull, 16384};
        connection.codeSyncUploaded = true;
        assert(connection.invalidateCodeCaches() == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
        assert(dispatches == 2 && connection.state == IOKitConnection::State::Ready);
        connection.hardwareQueues.clear();
        assert(connection.invalidateCodeCaches() == HSA_STATUS_SUCCESS && dispatches == 3);

        /* Any other failure still cannot prove completion: a session fault. */
        next_result = kIOReturnError;
        assert(connection.dispatch(request(), fence) == HSA_STATUS_ERROR);
        assert(connection.state == IOKitConnection::State::Faulted);
        connection.codeSyncBuffer = {};
    }
};
} }

int main() {
    mac_hsa::IdleDiagnosticAccess::run();
    assert(closes == 1);
    std::puts("Selector 51 NoResources and held queue slots leave the session healthy; other failures fault");
}
