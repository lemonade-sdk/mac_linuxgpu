/* Device power on the production IOKit transport (power.h). A driver that
 * is suspending, suspended or resuming refuses GPU work with
 * kIOReturnOffline before submitting anything: the transport returns
 * kDeviceSuspendedStatus and the session stays healthy, so the call can be
 * retried after resume. The power snapshot and requests go through
 * QueryInfo tag "LPWR" and selector 83, without the session lock, and a
 * driver that predates them is a clean decline. Only the IOKit calls are
 * replaced; no service is opened. */
#define MAC_HSA_IDLE_DIAGNOSTIC
#define IOServiceClose test_service_close
#define IOConnectCallScalarMethod test_scalar
#define IOConnectCallMethod test_method
#define mlg_fw_service_start_connection test_fw_start
#define mlg_fw_service_stop test_fw_stop
#include "../src/transport_iokit.cpp"
#include <cassert>
#include <cstdio>

extern "C" int test_fw_start(uint32_t, const char *, struct mlg_fw_service **) { assert(false); return -1; }
extern "C" void test_fw_stop(struct mlg_fw_service *service) { assert(!service); }

static constexpr io_connect_t test_port = 515;
static kern_return_t gpu_result = KERN_SUCCESS;   // what selectors 16/56/57/59/51 answer
static kern_return_t power_result = KERN_SUCCESS; // what tag LPWR and selector 83 answer
static uint64_t power_state = 0, power_generation = 1, last_power_op = UINT64_MAX;
static unsigned gpu_calls;

extern "C" kern_return_t test_service_close(io_connect_t port) {
    assert(port == test_port); return KERN_SUCCESS;
}
extern "C" kern_return_t test_scalar(mach_port_t port, uint32_t selector, const uint64_t *in,
    uint32_t inputs, uint64_t *out, uint32_t *count) {
    assert(port == test_port);
    if (selector == 43) {
        assert(!inputs && *count == 3);
        out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 219;
        return KERN_SUCCESS;
    }
    if ((selector == 21 && inputs == 1 && in[0] == amdgpu::power::kQueryTag) || selector == 83) {
        assert(inputs == 1 && *count == amdgpu::power::kWords);
        if (power_result != KERN_SUCCESS) return power_result;
        if (selector == 83) last_power_op = in[0];
        for (uint32_t i = 0; i < *count; ++i) out[i] = 0;
        out[amdgpu::power::Version] = 1;
        out[amdgpu::power::State] = power_state;
        out[amdgpu::power::Generation] = power_generation;
        out[amdgpu::power::Flags] = amdgpu::power::VRAMPreserved;
        return KERN_SUCCESS;
    }
    ++gpu_calls;
    if (gpu_result != KERN_SUCCESS) return gpu_result;
    switch (selector) {
    case 16: out[0] = 0x10001; out[1] = 0x200000000ull; out[2] = 0; return KERN_SUCCESS; // BOAlloc
    case 56: out[0] = 0; out[1] = 9; return KERN_SUCCESS;                               // AQLQueueCreate
    case 57: out[0] = 0; return KERN_SUCCESS;                                           // AQLQueueKick
    case 59: out[0] = 0; out[1] = 0; return KERN_SUCCESS;                               // AQLQueueService
    default: assert(false); return kIOReturnUnsupported;
    }
}
extern "C" kern_return_t test_method(mach_port_t port, uint32_t selector, const uint64_t *, uint32_t,
    const void *, size_t, uint64_t *out, uint32_t *count, void *, size_t *) {
    assert(port == test_port && selector == 51 && *count == 3);
    ++gpu_calls;
    if (gpu_result != KERN_SUCCESS) return gpu_result;
    out[0] = 0; out[1] = 1; out[2] = 3;
    return KERN_SUCCESS;
}

namespace mac_hsa { namespace {
struct IdleDiagnosticAccess {
    static void run() {
        IOKitConnection connection(OriginalAtomicCaps{}, true);
        connection.ownerPort = test_port;
        connection.state = IOKitConnection::State::Ready;
        connection.queueSlotLimit = 4;
        connection.capacity = 1ull << 30;
        const auto ready = [&] { return connection.state == IOKitConnection::State::Ready; };

        /* Suspended: every GPU call is refused, nothing submitted, healthy. */
        gpu_result = kIOReturnOffline;
        connection.hardwareQueues.emplace(9, std::array<uint64_t, 2>{1, 2});
        assert(connection.kickQueue(9, 5) == kDeviceSuspendedStatus && ready());
        uint64_t inactive = 7;
        assert(connection.serviceQueue(9, inactive) == kDeviceSuspendedStatus && ready() && !inactive);
        DeviceBuffer buffer;
        assert(connection.allocateRaw(16384, 3, buffer) == kDeviceSuspendedStatus && ready() && !buffer.handle);
        amdgpu::ComputeDispatchRequest request{};
        request.version = 2; request.codeHandle = 0x10000; request.codeBytes = sizeof(uint32_t);
        request.groups[0] = request.groups[1] = request.groups[2] = 1;
        request.threads[0] = 32; request.threads[1] = request.threads[2] = 1;
        request.rsrc1 = 0xc0000; request.timeoutUS = 100000;
        uint64_t fence = 0;
        assert(connection.dispatch(request, fence) == kDeviceSuspendedStatus && ready() && !fence);
        assert(gpu_calls == 4);

        /* Resumed: the same calls go through. */
        gpu_result = KERN_SUCCESS;
        assert(connection.kickQueue(9, 5) == HSA_STATUS_SUCCESS);
        assert(connection.serviceQueue(9, inactive) == HSA_STATUS_SUCCESS);
        assert(connection.dispatch(request, fence) == HSA_STATUS_SUCCESS && fence == 1);

        /* The power snapshot and requests. */
        PowerSnapshot snapshot;
        power_state = uint64_t(amdgpu::power::PowerState::Suspended);
        power_generation = 4;
        assert(connection.powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.valid());
        assert(snapshot.state() == amdgpu::power::PowerState::Suspended && snapshot.generation() == 4);
        assert(connection.requestPower(amdgpu::power::Prepare, snapshot) == HSA_STATUS_SUCCESS &&
               last_power_op == amdgpu::power::Prepare);
        assert(connection.requestPower(amdgpu::power::Resume, snapshot) == HSA_STATUS_SUCCESS &&
               last_power_op == amdgpu::power::Resume);
        assert(connection.requestPower(amdgpu::power::Query, snapshot) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        /* A malformed snapshot is an error, not a state. */
        power_state = 9;
        assert(connection.powerState(snapshot) == HSA_STATUS_ERROR);
        /* A driver that predates the protocol declines cleanly. */
        power_result = kIOReturnBadArgument;
        assert(connection.powerState(snapshot) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        power_result = kIOReturnUnsupported;
        assert(connection.requestPower(amdgpu::power::Prepare, snapshot) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(ready());

        /* A closed session (after a host sleep) is a fault as before; the
         * power state is still readable and says lost. */
        power_result = KERN_SUCCESS;
        power_state = uint64_t(amdgpu::power::PowerState::Lost);
        gpu_result = kIOReturnNotOpen;
        assert(connection.kickQueue(9, 6) == HSA_STATUS_ERROR && !ready());
        assert(connection.powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.lost());
        connection.ownerPort = IO_OBJECT_NULL;
        assert(connection.powerState(snapshot) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    }
};
}}

int main() {
    mac_hsa::IdleDiagnosticAccess::run();
    std::puts("IOKit transport device power: offline refusals keep the session, snapshot and requests, old-driver decline");
    return 0;
}
