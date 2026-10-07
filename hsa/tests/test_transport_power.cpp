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
/* Doorbells are sent async and answered later (kickQueue, drainKicksLocked). */
#define IOConnectCallAsyncScalarMethod test_async_scalar
#define mach_msg test_mach_msg
#define IODispatchCalloutFromMessage test_dispatch_callout
#define mlg_fw_service_start_connection test_fw_start
#define mlg_fw_service_stop test_fw_stop
/* The doorbell slice and gate a client of build 264 maps. */
#define IOConnectMapMemory64 test_map_memory
#define IOConnectUnmapMemory64 test_unmap_memory
/* The IOKit calls are replaced: every selector reaches them synchronously. */
#define MLG_SELECTOR_CALL_TEST_SYNC
#include "../src/transport_iokit.cpp"
#include <cassert>
#include <cstdio>
#include <deque>

extern "C" int test_fw_start(uint32_t, const char *, struct mlg_fw_service **) { assert(false); return -1; }
extern "C" void test_fw_stop(struct mlg_fw_service *service) { assert(!service); }

static constexpr io_connect_t test_port = 515;
static kern_return_t gpu_result = KERN_SUCCESS;   // what selectors 16/56/57/59/51 answer
static kern_return_t power_result = KERN_SUCCESS; // what tag LPWR and selector 83 answer
static uint64_t power_state = 0, power_generation = 1, last_power_op = UINT64_MAX;
static unsigned gpu_calls;
static uint64_t driver_build = 219;          // RuntimeBuild's out[2]
static kern_return_t sync_kick_result = KERN_SUCCESS; // selector 57 called synchronously (build 263)
static unsigned sync_kicks;
/* Build 264: AQLQueueCreate's doorbell (out[2]), the slice and gate maps. */
static uint64_t create_doorbell = UINT64_MAX;
static unsigned destroys, maps, unmaps;
static kern_return_t map_result = KERN_SUCCESS;
alignas(16384) static uint64_t test_slice[2048];
alignas(16384) static mlg_doorbell_gate test_gate;
extern "C" kern_return_t test_map_memory(io_connect_t port, uint32_t type, task_port_t, mach_vm_address_t *at,
    mach_vm_size_t *size, IOOptionBits options) {
    assert(port == test_port && (type == MLG_DOORBELL_MEMORY_TYPE || type == MLG_DOORBELL_GATE_MEMORY_TYPE));
    ++maps;
    if (map_result != KERN_SUCCESS) return map_result;
    if (type == MLG_DOORBELL_MEMORY_TYPE) {
        /* Device memory: never cached. */
        assert((options & kIOMapCacheMask) == kIOMapInhibitCache && (options & kIOMapAnywhere));
        *at = reinterpret_cast<uintptr_t>(test_slice); *size = sizeof(test_slice);
    } else {
        *at = reinterpret_cast<uintptr_t>(&test_gate); *size = 16384;
    }
    return KERN_SUCCESS;
}
extern "C" kern_return_t test_unmap_memory(io_connect_t port, uint32_t type, task_port_t, mach_vm_address_t at) {
    assert(port == test_port);
    assert((type == MLG_DOORBELL_MEMORY_TYPE && at == reinterpret_cast<uintptr_t>(test_slice)) ||
           (type == MLG_DOORBELL_GATE_MEMORY_TYPE && at == reinterpret_cast<uintptr_t>(&test_gate)));
    ++unmaps;
    return KERN_SUCCESS;
}

extern "C" kern_return_t test_service_close(io_connect_t port) {
    assert(port == test_port); return KERN_SUCCESS;
}
extern "C" kern_return_t test_scalar(mach_port_t port, uint32_t selector, const uint64_t *in,
    uint32_t inputs, uint64_t *out, uint32_t *count) {
    assert(port == test_port);
    if (selector == 43) {
        assert(!inputs && *count == 3);
        out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = driver_build;
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
    case 56:                                                                            // AQLQueueCreate
        /* From build 264 the client asks for the queue's doorbell too. */
        assert(*count == (driver_build >= 264 ? 3u : 2u));
        out[0] = 0; out[1] = 9;
        if (*count == 3) out[2] = create_doorbell;
        return KERN_SUCCESS;
    case 58: ++destroys; out[0] = 0; return KERN_SUCCESS;                               // AQLQueueDestroy
    case 57:                                                                            // AQLQueueKick, sync (263)
        assert(driver_build >= 263 && inputs == 2 && in[0] == 9 && *count == 1);
        ++sync_kicks;
        if (sync_kick_result != KERN_SUCCESS) return sync_kick_result;
        out[0] = 0; return KERN_SUCCESS;
    case 59: out[0] = 0; out[1] = 0; return KERN_SUCCESS;                               // AQLQueueService
    default: assert(false); return kIOReturnUnsupported;
    }
}
/* A doorbell: the delivery thread queues it (or refuses a dead session at
 * once); the session queue's answer, gpu_result, arrives as a completion. */
struct KickAnswer { io_user_reference_t function, refcon; uint64_t token; kern_return_t result; };
static std::deque<KickAnswer> kick_answers;
static uint64_t kick_token = 100;
extern "C" kern_return_t test_async_scalar(mach_port_t port, uint32_t selector, mach_port_t wake,
    uint64_t *reference, uint32_t references, const uint64_t *in, uint32_t inputs, uint64_t *out,
    uint32_t *count) {
    assert(port == test_port && selector == 57 && wake && references == kIOAsyncCalloutCount &&
           inputs == 2 && in[0] == 9 && *count == 1);
    ++gpu_calls;
    if (gpu_result == kIOReturnNotAttached || gpu_result == MACH_SEND_INVALID_DEST) return gpu_result;
    out[0] = ++kick_token;
    kick_answers.push_back({reference[kIOAsyncCalloutFuncIndex], reference[kIOAsyncCalloutRefconIndex],
                            out[0], gpu_result});
    return KERN_SUCCESS;
}
extern "C" mach_msg_return_t test_mach_msg(mach_msg_header_t *, mach_msg_option_t option, mach_msg_size_t,
    mach_msg_size_t, mach_port_name_t, mach_msg_timeout_t timeout, mach_port_name_t) {
    assert((option & MACH_RCV_MSG) && (option & MACH_RCV_TIMEOUT) && !timeout);
    return kick_answers.empty() ? MACH_RCV_TIMED_OUT : MACH_MSG_SUCCESS;
}
extern "C" void test_dispatch_callout(void *, mach_msg_header_t *, void *) {
    assert(!kick_answers.empty());
    const auto answer = kick_answers.front();
    kick_answers.pop_front();
    void *args[5] = {reinterpret_cast<void *>(uintptr_t(answer.token)),
                     reinterpret_cast<void *>(uintptr_t(uint32_t(answer.result))),
                     reinterpret_cast<void *>(uintptr_t(answer.result == KERN_SUCCESS)), nullptr, nullptr};
    reinterpret_cast<IOAsyncCallback>(answer.function)(reinterpret_cast<void *>(answer.refcon),
                                                         kIOReturnSuccess, args, 5);
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
        /* A doorbell is queued, not waited for: the refusal arrives with
         * its answer and is handed to the queue, which rings it on resume. */
        assert(connection.kickQueue(9, 5) == HSA_STATUS_SUCCESS && ready());
        uint64_t refused = 0;
        assert(connection.takeRefusedKick(9, refused) && refused == 5 && ready());
        assert(!connection.takeRefusedKick(9, refused));
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
        assert(connection.serviceQueue(9, inactive) == HSA_STATUS_SUCCESS && kick_answers.empty());
        assert(!connection.takeRefusedKick(9, refused) && ready());
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
        {
            /* A driver from build 263 rings the doorbell on its delivery
             * thread: one synchronous call, no async one. What it cannot
             * vouch for (Unsupported) goes as the async call; Offline waits
             * for resume; VMError is the process's memory fault. */
            IOKitConnection sync(OriginalAtomicCaps{}, true);
            sync.ownerPort = test_port;
            sync.state = IOKitConnection::State::Ready;
            sync.hardwareQueues.emplace(9, std::array<uint64_t, 2>{1, 2});
            driver_build = 263;
            const auto asyncBefore = kick_answers.size();
            assert(sync.kickQueue(9, 11) == HSA_STATUS_SUCCESS && sync_kicks == 1);
            assert(kick_answers.size() == asyncBefore);
            sync_kick_result = kIOReturnUnsupported;
            assert(sync.kickQueue(9, 12) == HSA_STATUS_SUCCESS && sync_kicks == 2);
            assert(kick_answers.size() == asyncBefore + 1);
            uint64_t idle = 0;
            assert(sync.serviceQueue(9, idle) == HSA_STATUS_SUCCESS && kick_answers.empty());
            sync_kick_result = kIOReturnOffline;
            assert(sync.kickQueue(9, 13) == kDeviceSuspendedStatus &&
                   sync.state == IOKitConnection::State::Ready);
            sync_kick_result = kIOReturnVMError;
            assert(sync.kickQueue(9, 14) == kMemoryFaultStatus);
            sync_kick_result = kIOReturnNotAttached;
            assert(sync.kickQueue(9, 15) == HSA_STATUS_ERROR && sync.state == IOKitConnection::State::Faulted);
            sync_kick_result = KERN_SUCCESS;
            driver_build = 219;
        }
        {
            /* Build 264: the client rings its KFD queue itself. createQueue
             * maps the doorbell slice (uncached) and the gate once, and the
             * queue's doorbell is one store into the slice, with no driver
             * call; through a closed gate nothing is written (the queue then
             * asks the driver, as below). A queue with no doorbell of its own
             * (MLG_DOORBELL_NONE) maps nothing; a slice that cannot be
             * mapped destroys the queue again and fails loudly. */
            driver_build = 264;
            const auto make = [](IOKitConnection &c, uint64_t &handle) {
                SharedBuffer ring, metadata;
                ring.device = {0x10010, 0x300000000ull, 16384}; ring.host = reinterpret_cast<void *>(0x300000000ull);
                metadata.device = {0x10011, 0x300004000ull, 16384}; metadata.host = reinterpret_cast<void *>(0x300004000ull);
                c.sharedBuffers[ring.device.handle] = ring;
                c.sharedBuffers[metadata.device.handle] = metadata;
                const auto status = c.createQueue(ring, metadata, 64, handle);
                c.sharedBuffers.clear();
                return status;
            };
            {
                IOKitConnection direct(OriginalAtomicCaps{}, true);
                direct.ownerPort = test_port;
                direct.state = IOKitConnection::State::Ready;
                direct.queueSlotLimit = 4;
                test_gate = {}; test_gate.magic = MLG_DOORBELL_GATE_MAGIC; test_gate.open = 1;
                create_doorbell = 16;
                uint64_t handle = 0;
                assert(make(direct, handle) == HSA_STATUS_SUCCESS && handle == 9 && maps == 2);
                const auto bell = direct.directDoorbell(9);
                assert(bell.gate == &test_gate && bell.doorbell == &test_slice[2]);
                const auto syncBefore = sync_kicks, callsBefore = gpu_calls;
                assert(mlg_doorbell_ring(bell.gate, bell.doorbell, 21) && test_slice[2] == 21 && !test_gate.busy);
                test_gate.open = 0;
                assert(!mlg_doorbell_ring(bell.gate, bell.doorbell, 22) && test_slice[2] == 21);
                assert(sync_kicks == syncBefore && gpu_calls == callsBefore);
                /* Destroyed: never handed out again. */
                assert(direct.destroyQueue(9) == HSA_STATUS_SUCCESS && destroys == 1);
                assert(!direct.directDoorbell(9).doorbell);
                /* No doorbell of its own: nothing mapped again, rung by the driver. */
                create_doorbell = MLG_DOORBELL_NONE;
                assert(make(direct, handle) == HSA_STATUS_SUCCESS && maps == 2 && !direct.directDoorbell(9).doorbell);
                assert(direct.destroyQueue(9) == HSA_STATUS_SUCCESS);
                /* A doorbell outside the slice is refused, the queue destroyed. */
                create_doorbell = sizeof(test_slice);
                const auto destroyed = destroys;
                assert(make(direct, handle) == HSA_STATUS_ERROR && destroys == destroyed + 1);
                assert(!direct.hardwareQueues.contains(9) && direct.state == IOKitConnection::State::Ready);
            }
            assert(unmaps == 2);	/* both maps go with the connection */
            {
                IOKitConnection unmapped(OriginalAtomicCaps{}, true);
                unmapped.ownerPort = test_port;
                unmapped.state = IOKitConnection::State::Ready;
                unmapped.queueSlotLimit = 4;
                map_result = kIOReturnNoMemory;
                create_doorbell = 16;
                uint64_t handle = 0;
                const auto destroyed = destroys;
                assert(make(unmapped, handle) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !handle);
                assert(destroys == destroyed + 1 && !unmapped.hardwareQueues.contains(9));
                map_result = KERN_SUCCESS;
            }
            create_doorbell = UINT64_MAX;
            driver_build = 219;
        }
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
        /* The doorbell is queued; its answer faults the session, which the
         * queue service reports. */
        assert(connection.kickQueue(9, 6) == HSA_STATUS_SUCCESS);
        assert(connection.serviceQueue(9, inactive) == HSA_STATUS_ERROR && !ready());
        assert(connection.powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.lost());
        /* The GPU left the bus: the driver's session (or the driver) is
         * gone, so calls fail with kIOReturnNotAttached or a dead port. The
         * device is lost, and the power state says so with the removal as
         * its cause, a new generation each time the driver went away. */
        gpu_result = kIOReturnNotAttached;
        connection.state = IOKitConnection::State::Ready;
        /* The call faults the session (queue errors then ask the power
         * state, which reports the loss: deviceStatus). */
        assert(connection.kickQueue(9, 7) == HSA_STATUS_ERROR && !ready());
        power_result = kIOReturnNotAttached;
        power_generation = 6;
        assert(connection.powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.valid() && snapshot.lost());
        assert(snapshot.words[amdgpu::power::Cause] == amdgpu::power::kCauseDeviceRemoved);
        assert(snapshot.flags() & amdgpu::power::LinkDown);
        assert(snapshot.generation() == 5);	/* after the last one the driver reported (4) */
        power_result = MACH_SEND_INVALID_DEST;
        assert(connection.powerState(snapshot) == HSA_STATUS_SUCCESS && snapshot.lost());
        connection.ownerPort = IO_OBJECT_NULL;
        assert(connection.powerState(snapshot) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    }
};
}}

int main() {
    mac_hsa::IdleDiagnosticAccess::run();
    std::puts("IOKit transport device power: offline refusals keep the session, snapshot and requests, old-driver decline, device removed reported lost");
    return 0;
}
