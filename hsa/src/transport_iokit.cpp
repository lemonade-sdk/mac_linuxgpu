#include "device_init.h"
#include "shim_init_diagnostics.h"
#include "transport_fake.h"
#include "../abi/amdgpu_vram_accounting.h"
#include "fw_mailbox_service.h"
#include "selector_call.h"
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include "mach_vm_compat.h"
#include "host_window.h"
#include "allocation_census.h"
#include <atomic>
#include <new>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

// A session client cannot attach while the driver closes a session, retires
// or stops: NewUserClient refuses it with kIOReturnNotAttached. Ask an
// observer (which may attach then) why, wait while a close is in progress,
// and fail with a status that says what is wrong rather than an invalid
// agent: kDeviceLostStatus for a session that will not close (quarantine,
// restart required, wedged GPU, device removed), HSA_STATUS_ERROR_RESOURCE_BUSY
// (kDeviceSuspendedStatus's value: busy, retry) for a retiring driver or a
// close still running at the bound.
static uint64_t sessionFlags(io_service_t service) {
    io_connect_t observer = IO_OBJECT_NULL;
    if (IOServiceOpen(service, mach_task_self(), MLG_USER_CLIENT_OBSERVER, &observer) != KERN_SUCCESS)
        return 0;
    const uint64_t tag = MLG_QUERY_SESSION_STATE;
    uint64_t state[MLG_SESSION_STATE_WORDS] = {};
    uint32_t count = MLG_SESSION_STATE_WORDS;
    const auto kr = IOConnectCallScalarMethod(observer, MLG_SELECTOR_QUERY_INFO, &tag, 1, state, &count);
    IOServiceClose(observer);
    return kr == KERN_SUCCESS && count >= 2 && state[0] >= 1 ? state[1] : 0;
}
static uint32_t sessionCloseWaitMs() {
    const char *value = std::getenv("MAC_HSA_SESSION_CLOSE_WAIT_MS");
    if (!value || !*value) return 60000;
    return uint32_t(std::strtoul(value, nullptr, 10));
}
static hsa_status_t openSessionClient(io_service_t service, io_connect_t &port) {
    auto kr = IOServiceOpen(service, mach_task_self(), 0, &port);
    if (kr == KERN_SUCCESS) return HSA_STATUS_SUCCESS;
    if (kr != kIOReturnNotAttached) return HSA_STATUS_ERROR_INVALID_AGENT;
    const uint32_t boundMs = sessionCloseWaitMs();
    const auto start = std::chrono::steady_clock::now();
    bool reported = false;
    for (;;) {
        const uint64_t flags = sessionFlags(service);
        if (flags & (MLG_SESSION_FLAG_QUARANTINED | MLG_SESSION_FLAG_RESTART_REQUIRED |
                     MLG_SESSION_FLAG_GPU_WEDGED | MLG_SESSION_FLAG_DEVICE_REMOVED)) {
            std::fprintf(stderr, "mac_linuxgpu: the driver's last session did not close cleanly "
                "(session flags %#llx); the GPU needs the driver restarted or a power cycle\n",
                (unsigned long long)flags);
            return mac_hsa::kDeviceLostStatus;
        }
        if (flags & MLG_SESSION_FLAG_RETIRING) {
            std::fprintf(stderr, "mac_linuxgpu: the driver is being retired (an upgrade): no new "
                "session until the new driver runs\n");
            return mac_hsa::kDeviceSuspendedStatus;  // HSA_STATUS_ERROR_RESOURCE_BUSY
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (waited >= boundMs) {
            std::fprintf(stderr, "mac_linuxgpu: the driver is still closing a session after %.1f s "
                "(session flags %#llx); retry when it has closed\n", boundMs / 1000.0,
                (unsigned long long)flags);
            return mac_hsa::kDeviceSuspendedStatus;  // HSA_STATUS_ERROR_RESOURCE_BUSY
        }
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "mac_linuxgpu: the driver is closing a session; waiting for it "
                "(up to %.1f s)\n", boundMs / 1000.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        kr = IOServiceOpen(service, mach_task_self(), 0, &port);
        if (kr == KERN_SUCCESS) return HSA_STATUS_SUCCESS;
        if (kr != kIOReturnNotAttached) return HSA_STATUS_ERROR_INVALID_AGENT;
    }
}

// The driver's device spec (session_state.h) in mac_hsa_device_spec_t's
// 32 words (mac_hsa.h): a group the driver did not report stays zero; the
// CU bitmap words carry two shader arrays per engine for four engines.
static void deviceSpecWords(const mlg_device_spec &spec, std::array<uint64_t, mac_hsa::kDeviceSpecDwords> &w)
{
    w.fill(0);
    if (!(spec.present & MLG_DEVICE_SPEC_GEOMETRY)) return;  // header 0: GC not resolved
    w[0] = spec.version;
    const uint32_t geometry[8] = {spec.shader_engines, spec.shader_arrays_per_se, spec.backends_per_se,
                                  spec.cus_per_array, spec.wavefront_size, spec.max_waves_per_simd,
                                  spec.scratch_slots_per_cu, spec.lds_bytes};
    for (unsigned i = 0; i < 8; ++i) w[1 + i] = geometry[i];
    if (spec.present & MLG_DEVICE_SPEC_CUS) {
        w[9] = spec.active_cus;
        for (unsigned se = 0; se < 4; ++se)
            for (unsigned sa = 0; sa < 2; ++sa) w[10 + se * 2 + sa] = spec.cu_bitmap[se][sa];
    }
    if (spec.present & MLG_DEVICE_SPEC_SHADER_ARRAYS) w[18] = spec.active_sa_bitmap;
    if (spec.present & MLG_DEVICE_SPEC_SA_DISABLE) {
        w[19] = spec.cc_sa_disable;
        w[20] = spec.user_sa_disable;
    }
    if (spec.present & MLG_DEVICE_SPEC_BACKENDS) {
        w[21] = spec.active_rb_bitmap;
        w[22] = spec.active_rbs;
    }
}

// Whether each open connection's driver serves session calls async
// (host/selector_call.h): checked on its first async call, forgotten when
// it closes.
static std::mutex &sessionCallProtocolsLock = *new std::mutex;
static std::unordered_map<io_connect_t, int> &sessionCallProtocols = *new std::unordered_map<io_connect_t, int>;

static kern_return_t closeConnection(io_connect_t port) {
    {
        std::lock_guard lock(sessionCallProtocolsLock);
        sessionCallProtocols.erase(port);
    }
    return IOServiceClose(port);
}

// A selector as the driver serves it (host/selector_call.h). Against a
// driver older than build 243 a session call fails at once, with the
// reason on stderr, never waiting for a completion that will not come.
static kern_return_t rpcMethod(io_connect_t port, uint32_t selector, const uint64_t *input,
                               uint32_t inputs, const void *inputStruct, size_t inputStructSize,
                               uint64_t *output, uint32_t *outputs, void *outputStruct,
                               size_t *outputStructSize) {
    int state;
    {
        std::lock_guard lock(sessionCallProtocolsLock);
        state = sessionCallProtocols[port];
    }
    const int before = state;
    const auto result = mlg_selector_call_on(port, &state, selector, input, inputs, inputStruct,
                                             inputStructSize, output, outputs, outputStruct,
                                             outputStructSize);
    if (state != before) {
        std::lock_guard lock(sessionCallProtocolsLock);
        sessionCallProtocols[port] = state;
        if (state < 0)
            std::fprintf(stderr, "mac_linuxgpu: the installed MacLinuxGPU driver is older than build %u "
                         "and this runtime needs it: install the matching driver\n",
                         MLG_SESSION_CALLS_ASYNC_BUILD);
    }
    return result;
}

namespace mac_hsa {
namespace {
class IOObject {
public:
    explicit IOObject(io_object_t value) : value_(value) {}
    ~IOObject() { if (value_) IOObjectRelease(value_); }
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
private:
    io_object_t value_;
};

std::optional<uint64_t> registryNumber(io_registry_entry_t entry,CFStringRef name) {
    const auto value=IORegistryEntryCreateCFProperty(entry,name,kCFAllocatorDefault,0);
    if (!value) return {};
    std::optional<uint64_t> result;
    if (CFGetTypeID(value)==CFNumberGetTypeID()) {
        int64_t number=0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&number) && number>=0)
            result=uint64_t(number);
    } else if (CFGetTypeID(value)==CFDataGetTypeID()) {
        const auto data=static_cast<CFDataRef>(value);const auto size=CFDataGetLength(data);
        if (size>0 && size<=8) {
            uint64_t number=0;const auto *bytes=CFDataGetBytePtr(data);
            for (CFIndex i=0;i<size;++i) number|=uint64_t(bytes[i])<<(i*8);
            result=number;
        }
    }
    CFRelease(value);return result;
}

OriginalAtomicCaps captureOriginalAtomicCaps(io_service_t service) {
    OriginalAtomicCaps audit{};audit.captured=true;
    io_registry_entry_t entry=service;IOObjectRetain(entry);
    bool complete=false;
    for (unsigned depth=0;entry && depth<64;++depth) {
        const auto vendor=registryNumber(entry,CFSTR("vendor-id"));
        const auto flags=registryNumber(entry,CFSTR("IOPCIExpressCapabilities"));
        if (vendor || flags || IOObjectConformsTo(entry,"IOPCIDevice")) {
            if (audit.count==audit.functions.size()) {IOObjectRelease(entry);entry=0;break;}
            auto &node=audit.functions[audit.count++];
            IORegistryEntryGetRegistryEntryID(entry,&node.registryID);
            node.vendorID=uint32_t(vendor.value_or(0));
            node.deviceID=uint32_t(registryNumber(entry,CFSTR("device-id")).value_or(0));
            const auto caps=registryNumber(entry,CFSTR("IOPCIExpressDeviceCapabilities2"));
            const auto control=registryNumber(entry,CFSTR("IOPCIExpressDeviceControl2"));
            node.expressKnown=flags && *flags<UINT16_MAX;
            node.capabilities2Known=caps && *caps<UINT32_MAX;
            node.control2Known=control && *control<UINT16_MAX;
            if(node.expressKnown) node.expressCapabilities=uint16_t(*flags);
            if(node.capabilities2Known) node.capabilities2=uint32_t(*caps);
            if(node.control2Known) node.control2=uint16_t(*control);
            if(node.expressKnown && ((node.expressCapabilities>>4)&15)==4) {
                complete=true;IOObjectRelease(entry);entry=0;break;
            }
        }
        io_registry_entry_t parent=0;
        const auto status=IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent);
        IOObjectRelease(entry);entry=0;
        if(status!=KERN_SUCCESS) break;
        entry=parent;
    }
    if(entry) IOObjectRelease(entry);
    assessOriginalAtomicCaps(audit,complete);return audit;
}

class IOKitConnection final : public Connection, private ShimInitializationRPC {
#if defined(MAC_HSA_IDLE_DIAGNOSTIC)
    friend struct IdleDiagnosticAccess;
#endif
public:
    explicit IOKitConnection(OriginalAtomicCaps audit, bool linuxShim = false)
        : originalAtomicCaps(std::move(audit)), linuxShim(linuxShim) {}
    io_service_t service = IO_OBJECT_NULL;
    uint64_t registryID = 0;
    const OriginalAtomicCaps originalAtomicCaps;
    ~IOKitConnection() override {
        // FinishStop resets or quarantines resources before releasing backing.
        for (const auto &[handle, buffer] : sharedBuffers) {
            (void)handle;
            IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), reinterpret_cast<uintptr_t>(buffer.host));
        }
        stopFirmwareService(); // Never left running past initialization.
        if (ownerPort) closeConnection(ownerPort);
        if (pendingProbePort) closeConnection(pendingProbePort);
        if (service) IOObjectRelease(service);
    }
    bool supportsBuffers() const override { return true; }
    hsa_status_t properties(DeviceProperties &out) override {
        std::lock_guard lock(sessionMutex);
        auto status=ensureReady();
        if (status!=HSA_STATUS_SUCCESS) return status;
        std::array<uint64_t,3> build{};
        status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<kQueueResourceDriverBuild) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const uint64_t tag=6;std::array<uint64_t,10> values{};
        status=scalar(21,{&tag,1},values);
        if (status!=HSA_STATUS_SUCCESS) return status;
        // Structural checks only (field widths, nonzero counts, the two wave
        // sizes AMDGPU defines); the values themselves are the device's.
        if (!values[0] || values[0]>UINT16_MAX || values[1]>UINT8_MAX || values[2]>UINT16_MAX ||
            values[3]>UINT32_MAX || !values[4] || values[4]>UINT16_MAX ||
            !values[5] || values[5]>UINT16_MAX || !values[6] || values[6]>UINT16_MAX ||
            values[7]>UINT32_MAX || !values[8] || values[8]>UINT16_MAX ||
            (values[9]!=32 && values[9]!=64))
            return HSA_STATUS_ERROR;
        DeviceProperties properties{};
        properties.chipID=uint32_t(values[0]);properties.revision=uint32_t(values[1]);
        properties.bdf=uint32_t(values[2]);properties.domain=uint32_t(values[3]);
        properties.computeUnits=uint32_t(values[4]);properties.shaderEngines=uint32_t(values[5]);
        properties.arraysPerEngine=uint32_t(values[6]);properties.timestampFrequency=values[7];
        properties.maxWavesPerCU=uint32_t(values[8]);properties.wavefrontSize=uint32_t(values[9]);
        if (linuxShim) {
            applyDeviceTopology(properties,topologyLocked());
            applyProductName(properties,productNameLocked());
        }
        out=properties;
        return HSA_STATUS_SUCCESS;
    }

    hsa_status_t spec(std::array<uint64_t, kDeviceSpecDwords> &out) override {
        // QueryInfo tag 8. The Linux-shim driver answers a structure
        // (session_state.h's struct mlg_device_spec, through OWNER_RESULT);
        // one that predates it declines (kIOReturnNotReady or
        // kIOReturnBadArgument), which is an invalid-argument decline here.
        std::lock_guard lock(sessionMutex);
        auto status=ensureReady();
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (linuxShim) {
            mlg_device_spec spec{};
            const uint64_t tag=MLG_QUERY_DEVICE_SPEC;
            uint64_t bytes=0;
            uint32_t count=1;
            size_t size=sizeof(spec);
            const auto result=rpcMethod(ownerPort,21,&tag,1,nullptr,0,&bytes,&count,&spec,&size);
            if (result==kIOReturnNotReady || result==kIOReturnBadArgument || result==kIOReturnUnsupported)
                return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            if (result!=KERN_SUCCESS) return HSA_STATUS_ERROR;
            if (size<4*sizeof(uint32_t) || spec.version<1 || spec.size!=size || (count && bytes!=size))
                return HSA_STATUS_ERROR;
            deviceSpecWords(spec,out);
            return HSA_STATUS_SUCCESS;
        }
        std::array<uint64_t,3> build{};
        status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<kDeviceSpecDriverBuild) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const uint64_t tag=8;std::array<uint64_t,1> input{tag};
        return scalar(21,input,out);
    }
    bool supportsSharedBuffers() const override { return true; }
    std::string memoryReport() override {
        std::lock_guard lock(sessionMutex);
        return census.report();
    }
    hsa_status_t sharedMemoryCapacity(uint64_t &bytes) override {
        std::lock_guard lock(sessionMutex);
        auto status = ensureReady();
        if (status == HSA_STATUS_SUCCESS) status = ensureHostWindow();
        if (status == HSA_STATUS_SUCCESS) bytes = sharedCapacityLocked();
        return status;
    }


    static hsa_status_t call(io_connect_t port, uint32_t selector, const uint64_t *input, uint32_t inputs,
                      uint64_t *output, uint32_t outputs, uint32_t *rawResult = nullptr,
                      uint32_t *actualCount = nullptr) {
        uint32_t count = outputs;
        const auto result = rpcMethod(port, selector, input, inputs, nullptr, 0,
                                      output, &count, nullptr, nullptr);
        if (rawResult) *rawResult = uint32_t(result);
        if (actualCount) *actualCount = count;
        if (selector == 60 && (result != KERN_SUCCESS || count != outputs))
            std::fprintf(stderr, "AtomicOp requester RPC: IOReturn=%#x output-count=%u expected=%u\n",
                         unsigned(result), count, outputs);
        // The driver's session or the driver itself is gone (the GPU left
        // the bus, or the driver stopped): the device is lost, not busy.
        // NoDevice is also Disconnect GPU closing this program's session.
        if (result == kIOReturnNoDevice) {
            static std::atomic_flag said = ATOMIC_FLAG_INIT;
            if (!said.test_and_set())
                std::fprintf(stderr, "mac_linuxgpu: the GPU was disconnected (Disconnect GPU, or it left the bus); "
                             "this program's GPU session is gone: restart it to use the GPU again\n");
        }
        if (result == kIOReturnNoDevice || result == kIOReturnNotAttached ||
            result == MACH_SEND_INVALID_DEST) return kDeviceLostStatus;
        // The device is suspending, suspended or resuming (power.h): the
        // driver submitted nothing; the call is retried after resume.
        if (result == kIOReturnOffline) return kDeviceSuspendedStatus;
        if (result == kIOReturnBusy || result == kIOReturnNoMemory || result == kIOReturnNoSpace || result == kIOReturnNoResources)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (result == kIOReturnBadArgument) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (result != KERN_SUCCESS || count != outputs) return HSA_STATUS_ERROR;
        return HSA_STATUS_SUCCESS;
    }

    hsa_status_t memoryCapacity(uint64_t &bytes) override {
        std::lock_guard lock(sessionMutex);
        const auto status = ensureReady();
        if (status == HSA_STATUS_SUCCESS) bytes = capacity;
        return status;
    }
    hsa_status_t memoryAvailable(uint64_t &bytes) override {
        std::lock_guard lock(sessionMutex);
        auto status = ensureReady();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (linuxShim) {
            const uint64_t tag = 9;
            std::array<uint64_t, 6> usage{};
            status = scalar(21, {&tag, 1}, usage);
            if (status != HSA_STATUS_SUCCESS) return status;
            if (!usage[0] || !usage[1] || usage[1] > usage[0] ||
                usage[2] > usage[0] || usage[3] > usage[1] ||
                !usage[4] || usage[4] > usage[0] || usage[5] > usage[4])
                return HSA_STATUS_ERROR;
            bytes = usage[3];
            return HSA_STATUS_SUCCESS;
        }
        std::array<uint64_t, 3> build{};
        status = scalar(43, {}, build);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 178) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const uint64_t tag = 5;
        amdgpu::vram_accounting::Snapshot accounting{};
        status = scalar(21, {&tag, 1}, accounting.values);
        if (status != HSA_STATUS_SUCCESS) return status;
        using namespace amdgpu::vram_accounting;
        if (!valid(accounting)) return HSA_STATUS_ERROR;
        if (!(accounting.values[Flags] & kValid)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        // The agent's VRAM pool allocates domain 3. This shared driver pool
        // accounts for every client and excludes visible/firmware reservations.
        bytes = accounting.values[DeviceFree];
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateBuffer(uint64_t bytes, DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        const auto status = ensureReady();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!bytes || bytes > capacity || bytes > UINT64_MAX - 16383)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        const auto allocated = allocateRaw((bytes + 16383) & ~uint64_t(16383), 3, buffer);
        if (allocated == HSA_STATUS_SUCCESS) {
            vramBytes += buffer.size; ++vramCount;
            census.add(buffer.handle, AllocationCensus::Kind::VRAM, buffer.size);
        } else reportAllocationFailureLocked("VRAM", bytes, allocated);
        return allocated;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (sharedBuffers.contains(buffer.handle)) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        const auto status = scalar(17, {&buffer.handle, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        else {
            vramBytes -= std::min(vramBytes, buffer.size); vramCount -= vramCount != 0;
            census.remove(buffer.handle);
        }
        return status;
    }
    hsa_status_t readBuffer(const DeviceBuffer &buffer, uint64_t offset, void *out, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        return transfer(buffer, offset, out, bytes, false);
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *in, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        return transfer(buffer, offset, const_cast<void *>(in), bytes, true);
    }
    hsa_status_t invalidateCodeCaches() override {
        // The existing native dispatch path brackets a bounded launch with
        // ACQUIRE_MEM (GLI/GLK/GLV/GL1/GL2 invalidation and writeback) and waits
        // for its EOP fence. A one-instruction utility kernel lets the loader
        // use that path on existing drivers without exposing arbitrary PM4.
        // Keep its small allocation for this connection's lifetime; it never
        // aliases a released executable and is reclaimed when ownerPort closes.
        std::lock_guard utilityLock(codeSyncMutex);
        // CodeSync (selector 90, kCodeSyncDriverBuild): the cache work
        // alone, a kernel-ring ACQUIRE_MEM waited for in the driver.
        if (!codeSyncSelectorKnown) {
            std::array<uint64_t,3> build{};
            const auto status = [&] { std::lock_guard lock(sessionMutex); return scalar(43, {}, build); }();
            if (status != HSA_STATUS_SUCCESS) return status;
            codeSyncSelector = build[2] >= kCodeSyncDriverBuild;
            codeSyncSelectorKnown = true;
        }
        if (codeSyncSelector) {
            std::lock_guard lock(sessionMutex);
            if (state != State::Ready) return HSA_STATUS_ERROR;
            const uint64_t timeoutUS = 100000;
            std::array<uint64_t,1> result{};
            const auto status = scalar(90, {&timeoutUS, 1}, result);
            if (status != HSA_STATUS_SUCCESS) return status;
            if (int64_t(result[0]) == -110) {
                std::fprintf(stderr, "mac_hsa: the GPU's cache invalidate after a code-object load did not "
                             "finish within %llu ms\n", (unsigned long long)(timeoutUS / 1000));
                return HSA_STATUS_ERROR;
            }
            return result[0] ? HSA_STATUS_ERROR : HSA_STATUS_SUCCESS;
        }
        if (!codeSyncBuffer.handle) {
            const auto status = allocateBuffer(sizeof(uint32_t), codeSyncBuffer);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        if (!codeSyncUploaded) {
            DeviceSnapshot snapshot;
            IsaTarget isa;
            auto status = read(snapshot);
            if (status != HSA_STATUS_SUCCESS) return status;
            if (!deviceIsa(snapshot, isa)) return HSA_STATUS_ERROR_INVALID_ISA;
            const uint32_t endProgram = endProgramEncoding(isa);
            status = writeBuffer(codeSyncBuffer, 0, &endProgram, sizeof(endProgram));
            if (status != HSA_STATUS_SUCCESS) return status;
            codeSyncUploaded = true;
        }
        amdgpu::ComputeDispatchRequest request{};
        request.version = 2;
        request.codeHandle = codeSyncBuffer.handle;
        request.codeBytes = sizeof(uint32_t);
        request.groups[0] = request.groups[1] = request.groups[2] = 1;
        request.threads[0] = 32;
        request.threads[1] = request.threads[2] = 1;
        request.rsrc1 = 0xc0000;
        request.timeoutUS = 100000;
        uint64_t fence = 0;
        return dispatch(request, fence);
    }
    hsa_status_t allocateSharedBuffer(uint64_t bytes, SharedBuffer &out) override {
        std::lock_guard lock(sessionMutex);
        return allocateSharedBufferLocked(bytes, out);
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        const auto found = sharedBuffers.find(buffer.device.handle);
        if (found == sharedBuffers.end() || found->second.host != buffer.host ||
            found->second.device.size != buffer.device.size || found->second.device.address != buffer.device.address ||
            found->second.memoryType != buffer.memoryType)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        for (const auto &[queue,handles]:hardwareQueues) {
            (void)queue;
            if (handles[0]==buffer.device.handle || handles[1]==buffer.device.handle)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        if (IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), reinterpret_cast<uintptr_t>(buffer.host)) != KERN_SUCCESS) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        hostReservation.give(buffer.device.address, buffer.device.size);
        sharedBytes -= std::min(sharedBytes, buffer.device.size);
        census.remove(buffer.device.handle);
        sharedBuffers.erase(found);
        const auto status = scalar(17, {&buffer.device.handle, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        return status;
    }
    hsa_status_t atomicRequesterExperiment(bool enable,amdgpu::atomic_requester::Snapshot &out) override {
        std::lock_guard lock(sessionMutex);
        auto status=enable ? ensureReady() : (ownerPort ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!hardwareQueues.empty()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t,3> build{};status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<191) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const uint64_t input=enable ? 1 : 0;amdgpu::atomic_requester::Snapshot snapshot{};
        status=scalar(60,{&input,1},{snapshot.values,amdgpu::atomic_requester::Count});
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!amdgpu::atomic_requester::valid(snapshot)) return HSA_STATUS_ERROR;
        out=snapshot;
        if (snapshot.values[amdgpu::atomic_requester::Status]) return HSA_STATUS_ERROR;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t sharedAtomicDiagnostics(const SharedBuffer &buffer,uint64_t offset,
        uint64_t queue,amdgpu::atomic_diag::Snapshot &out) override {
        std::lock_guard lock(sessionMutex);
        // Observational only: never initialize or recover a session here.
        if (state!=State::Ready || !ownerPort) return HSA_STATUS_ERROR;
        const auto found=sharedBuffers.find(buffer.device.handle);
        if (found==sharedBuffers.end() || found->second.host!=buffer.host ||
            found->second.device.address!=buffer.device.address || found->second.device.size!=buffer.device.size ||
            found->second.memoryType!=buffer.memoryType || (offset&7) ||
            offset>buffer.device.size || 8>buffer.device.size-offset)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (!queue || !hardwareQueues.contains(queue)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        std::array<uint64_t,3> build{};
        auto status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<190) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const std::array<uint64_t,4> input={7,buffer.device.handle,offset,queue};
        amdgpu::atomic_diag::Snapshot snapshot{};
        status=scalar(21,input,{snapshot.values,amdgpu::atomic_diag::Count});
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!amdgpu::atomic_diag::snapshot_valid(snapshot,buffer.device.address+offset)) return HSA_STATUS_ERROR;
        out=snapshot;return HSA_STATUS_SUCCESS;
    }
    bool queueSlotsExhausted() override {
        std::lock_guard lock(sessionMutex);
        return queueSlotsExhaustedLocked();
    }
    hsa_status_t createQueue(const SharedBuffer &ring,const SharedBuffer &metadata,uint32_t packets,uint64_t &handle) override {
        std::lock_guard lock(sessionMutex); handle=0;
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        for (const auto *buffer:{&ring,&metadata}) {
            const auto found=sharedBuffers.find(buffer->device.handle);
            if (found==sharedBuffers.end() || found->second.host!=buffer->host ||
                found->second.device.address!=buffer->device.address || found->second.device.size!=buffer->device.size)
                return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        }
        if (packets<kQueueMinPackets || packets>kQueueMaxPackets || (packets&(packets-1)) ||
            ring.device.size<uint64_t(packets)*64 || metadata.device.size<512 || ring.device.handle==metadata.device.handle)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t,3> build{};
        auto status=scalar(43,{},build);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (build[2]<kPersistentQueueDriverBuild) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        // A driver with no free slot fails the map without a distinguishable
        // status; never ask it when this session already holds every slot.
        if (queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        // Reserve bookkeeping before firmware can own the shared buffers.
        auto [record,inserted]=hardwareQueues.emplace(0,std::array<uint64_t,2>{ring.device.handle,metadata.device.handle});
        if (!inserted) return HSA_STATUS_ERROR;
        const std::array<uint64_t,3> input={ring.device.handle,metadata.device.handle,packets};
        std::array<uint64_t,2> output{};
        std::atomic_thread_fence(std::memory_order_seq_cst);
        status=scalar(56,input,output);
        // These RPC errors are returned before any queue map is attempted.
        // Exhausting the driver's queue slots must not poison existing queues.
        if (status==HSA_STATUS_ERROR_OUT_OF_RESOURCES || status==HSA_STATUS_ERROR_INVALID_ARGUMENT ||
            status==kDeviceSuspendedStatus) {
            hardwareQueues.erase(record);return status;
        }
        if (status!=HSA_STATUS_SUCCESS || output[0] || !output[1] || hardwareQueues.contains(output[1])) {
            state=State::Faulted;return HSA_STATUS_ERROR;
        }
        auto node=hardwareQueues.extract(record);node.key()=output[1];hardwareQueues.insert(std::move(node));
        handle=output[1];return HSA_STATUS_SUCCESS;
    }
    hsa_status_t kickQueue(uint64_t handle,uint64_t packet) override {
        std::lock_guard lock(sessionMutex);
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        if (!hardwareQueues.contains(handle) || !handle || packet==UINT64_MAX) return HSA_STATUS_ERROR_INVALID_QUEUE;
        const std::array<uint64_t,2> input={handle,packet};std::array<uint64_t,1> output{};
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint32_t raw=0;
        const auto status=call(ownerPort,57,input.data(),uint32_t(input.size()),output.data(),
                               uint32_t(output.size()),&raw);
        // The process's GPU work faulted: KFD evicted its queues. The
        // queue service call says where.
        if (raw==uint32_t(kIOReturnVMError)) {
            uint64_t inactive=0;
            (void)serviceQueueLocked(handle,inactive);
            return kMemoryFaultStatus;
        }
        if (status==kDeviceSuspendedStatus) return status; // nothing rung; replayed after resume
        if (status!=HSA_STATUS_SUCCESS || output[0]) {state=State::Faulted;return HSA_STATUS_ERROR;}
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t handle) override {
        std::lock_guard lock(sessionMutex);
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        if (!handle || !hardwareQueues.contains(handle)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        std::array<uint64_t,1> output{};
        const auto status=scalar(58,{&handle,1},output);
        if (status!=HSA_STATUS_SUCCESS || output[0]) {state=State::Faulted;return HSA_STATUS_ERROR;}
        std::atomic_thread_fence(std::memory_order_seq_cst);
        hardwareQueues.erase(handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t serviceQueue(uint64_t handle,uint64_t &inactive) override {
        std::lock_guard lock(sessionMutex);
        return serviceQueueLocked(handle,inactive);
    }
    bool memoryFault(MemoryFault &out) override {
        std::lock_guard lock(sessionMutex);
        if (!faulted) return false;
        out=fault;return true;
    }
    hsa_status_t serviceQueueLocked(uint64_t handle,uint64_t &inactive) {
        inactive=0;
        if (faulted) return kMemoryFaultStatus;
        if (state!=State::Ready) return HSA_STATUS_ERROR;
        if (!handle || !hardwareQueues.contains(handle)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        // A driver from kQueueFaultDriverBuild on also says where a GPU
        // memory fault of this process was. Asked once per connection.
        if (!serviceOutputs) {
            std::array<uint64_t,3> build{};
            const auto status=scalar(43,{},build);
            if (status!=HSA_STATUS_SUCCESS) return status;
            serviceOutputs=build[2]>=kQueueFaultDriverBuild ? 4 : 2;
        }
        std::array<uint64_t,4> output{};
        const auto status=scalar(59,{&handle,1},{output.data(),serviceOutputs});
        if (status==kDeviceSuspendedStatus) return status;
        if (status!=HSA_STATUS_SUCCESS) {state=State::Faulted;return status;}
        // The CP stopped the queue with an error code. A fault it hit is
        // reported as kIOReturnVMError once KFD's interrupt work signaled
        // the process's memory event, a little later: keep asking, up to
        // kQueueErrorSettle, then report the code.
        if (output[0]==uint32_t(kIOReturnIOError) && serviceOutputs==4) {
            const auto now=std::chrono::steady_clock::now();
            auto [entry,first]=queueErrorSince.try_emplace(handle,now);
            if (now-entry->second<kQueueErrorSettle) return HSA_STATUS_SUCCESS;
            std::fprintf(stderr,"mac_hsa: the GPU stopped queue %llu with error code %#llx "
                         "(no memory fault reported)\n",(unsigned long long)handle,
                         (unsigned long long)output[1]);
            return HSA_STATUS_ERROR_EXCEPTION;
        }
        if (output[0]==uint32_t(kIOReturnVMError)) {
            const uint64_t flags=output[2];
            faulted=true;
            fault.address=output[3];
            fault.reason=(flags&1 ? HSA_AMD_MEMORY_FAULT_PAGE_NOT_PRESENT : 0) |
                         (flags&2 ? HSA_AMD_MEMORY_FAULT_READ_ONLY : 0) |
                         (flags&4 ? HSA_AMD_MEMORY_FAULT_NX : 0) |
                         (flags&8 ? HSA_AMD_MEMORY_FAULT_IMPRECISE : 0);
            if (!(flags&(1ull<<31))) fault.reason|=HSA_AMD_MEMORY_FAULT_IMPRECISE;
            return kMemoryFaultStatus;
        }
        inactive=output[1];
        // A suspended queue can still be removed safely after a resource error.
        if (output[0]==uint32_t(kIOReturnNoMemory) || output[0]==uint32_t(kIOReturnNoResources))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (output[0]) return HSA_STATUS_ERROR_EXCEPTION;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &request, uint64_t &completion) override {
        std::lock_guard lock(sessionMutex);
        completion=UINT64_MAX;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!amdgpu::aql_dispatch_shape(request)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t,3> build{};
        const auto buildStatus=scalar(43,{},build);
        if (buildStatus != HSA_STATUS_SUCCESS) return buildStatus;
        if (build[2]<184) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        // The bounded launch borrows a queue slot for its duration.
        if (queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t,5> output{};
        uint32_t count=output.size();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status=rpcMethod(ownerPort,55,nullptr,0,&request,sizeof(request),
            output.data(),&count,nullptr,nullptr);
        // Linux-shim contract: kIOReturnNoResources means every queue slot is
        // held (possibly by another client) and the driver refused before
        // reserving one or touching hardware, so the session stays healthy.
        if (linuxShim && status == kIOReturnNoResources) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (status == kIOReturnOffline) return kDeviceSuspendedStatus;
        if (status != KERN_SUCCESS || count != output.size() || output[0] || output[1] ||
            output[2]!=5 || output[3] || output[4]!=1) {
            state=State::Faulted; return HSA_STATUS_ERROR;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        completion=output[1]; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &request, uint64_t &fence) override {
        std::lock_guard lock(sessionMutex);
        fence = 0;
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!amdgpu::compute_dispatch_shape(request, rsrc1ClampAndIEEE))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (request.version == 2) {
            std::array<uint64_t, 3> build{};
            const auto status = scalar(43, {}, build);
            if (status != HSA_STATUS_SUCCESS) return status;
            if (build[2] < 183) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        // The Linux-shim driver runs selector 51 as a one-shot AQL dispatch
        // that borrows a queue slot for its duration, like selector 55.
        if (linuxShim && queueSlotsExhaustedLocked()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t, 3> output{};
        uint32_t count = output.size();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status = rpcMethod(ownerPort, 51, nullptr, 0, &request,
            request.version == 1 ? amdgpu::kComputeDispatchV1Bytes : sizeof(request),
            output.data(), &count, nullptr, nullptr);
        // Linux-shim contract (as for 55): kIOReturnNoResources means every
        // queue slot is held and the driver refused before reserving one or
        // touching hardware. Nothing was submitted; the session stays healthy.
        if (linuxShim && status == kIOReturnNoResources) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (status == kIOReturnOffline) return kDeviceSuspendedStatus;
        if (status != KERN_SUCCESS || count != output.size() || output[0] ||
            output[2] != 3 || !output[1] || output[1] <= lastComputeFence) {
            // A failed or malformed response cannot prove completion. The
            // driver keeps code, arguments and referenced BOs until recovery.
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        fence = lastComputeFence = output[1];
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t testSharedAtomicAdd(const SharedBuffer &buffer, uint64_t offset,
                                    int64_t increment, uint32_t repetitions) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        const auto found = sharedBuffers.find(buffer.device.handle);
        if (found == sharedBuffers.end() || found->second.host != buffer.host ||
            found->second.device.address != buffer.device.address ||
            found->second.device.size != buffer.device.size || found->second.memoryType != buffer.memoryType)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (offset % 8 || offset > buffer.device.size || buffer.device.size - offset < 8 ||
            !repetitions || repetitions > 64) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::array<uint64_t, 4> ips{};
        const uint64_t tag = 3;
        auto status = scalar(21, {&tag, 1}, ips);
        if (status != HSA_STATUS_SUCCESS) return status;
        // Tag 3 word 1 is the SDMA0 IP version (major<<16 | minor<<8 | rev).
        // SDMA_PKT_ATOMIC (op 10) has one layout from SDMA 4.0 through 7.0,
        // the range ROCr's BuildAtomicDecrementCommand emits it for; SDMA 7.1
        // (GFX12.5) adds scope fields this diagnostic does not encode.
        const uint64_t sdmaMajor = (ips[1] >> 16) & 255, sdmaMinor = (ips[1] >> 8) & 255;
        if (sdmaMajor < 4 || sdmaMajor > 7 || (sdmaMajor == 7 && sdmaMinor >= 1))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        const std::array<uint64_t, 2> create{0, 0};
        uint64_t stream = 0;
        status = scalar(37, create, {&stream, 1});
        if (status != HSA_STATUS_SUCCESS) return status;
        const auto address = buffer.device.address + offset;
        const auto value = static_cast<uint64_t>(increment);
        // SDMA_PKT_ATOMIC / ADD64, as emitted by BuildAtomicDecrementCommand.
        const std::array<uint64_t, 10> packet{stream, 10u | (47u << 25),
            uint32_t(address), uint32_t(address >> 32), uint32_t(value), uint32_t(value >> 32), 0, 0, 0, 8};
        for (uint32_t i = 0; i < repetitions && status == HSA_STATUS_SUCCESS; ++i)
            status = scalar(38, packet, {});
        if (status != HSA_STATUS_SUCCESS) {
            if (scalar(39, {&stream, 1}, {}) != HSA_STATUS_SUCCESS) state = State::Faulted;
            return status;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint64_t fence = 0;
        status = scalar(19, {&stream, 1}, {&fence, 1});
        if (status != HSA_STATUS_SUCCESS) {
            // Submission may have partially reached hardware; retain all backing.
            state = State::Faulted; return status;
        }
        const std::array<uint64_t, 2> wait{fence, 1000000000};
        uint64_t timedOut = 1;
        status = scalar(20, wait, {&timedOut, 1});
        if (status != HSA_STATUS_SUCCESS || timedOut) {
            state = State::Faulted;
            return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        status = scalar(39, {&stream, 1}, {});
        if (status != HSA_STATUS_SUCCESS) state = State::Faulted;
        return status;
    }
    hsa_status_t copyBuffers(const DeviceBuffer &source, uint64_t sourceOffset,
        const DeviceBuffer &destination, uint64_t destinationOffset, size_t bytes) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!bytes || bytes > 4 * 1024 * 1024 || sourceOffset > source.size || bytes > source.size - sourceOffset ||
            destinationOffset > destination.size || bytes > destination.size - destinationOffset ||
            (source.handle == destination.handle && sourceOffset < destinationOffset + bytes &&
             destinationOffset < sourceOffset + bytes))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto status = copyRaw(source.handle, sourceOffset, destination.handle, destinationOffset, bytes);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return status;
    }
    hsa_status_t exportBuffer(const DeviceBuffer &buffer, BufferToken &token) override {
        std::lock_guard lock(sessionMutex);
        if (state != State::Ready) return HSA_STATUS_ERROR;
        std::array<uint64_t, 3> build{};
        auto status = scalar(43, {}, build);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 181) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::array<uint64_t, 3> input{buffer.handle, 0, 0}, output{};
        arc4random_buf(input.data() + 1, 16);
        status = scalar(52, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if ((!output[0] && !output[1]) || output[2] != buffer.size) return HSA_STATUS_ERROR;
        token = {registryID, {output[0], output[1]}, output[2]}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t importBuffer(const BufferToken &token, DeviceBuffer &buffer) override {
        std::lock_guard lock(sessionMutex);
        if (state == State::Faulted || pendingProbePort) return HSA_STATUS_ERROR;
        if (token.registryID != registryID || !token.size || token.size % 16384 || (!token.token[0] && !token.token[1]))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        // Probe before claiming: a stale token must not initialize/reset a GPU.
        io_connect_t probe = ownerPort;
        if (!probe) {
            if (const auto opened = openSessionClient(service, probe); opened != HSA_STATUS_SUCCESS)
                return opened;
        }
        std::array<uint64_t, 3> build{}; uint64_t tag = 4, stage = 0;
        auto status = call(probe, 43, nullptr, 0, build.data(), 3);
        if (status == HSA_STATUS_SUCCESS) status = call(probe, 21, &tag, 1, &stage, 1);
        if (probe != ownerPort && closeConnection(probe) != KERN_SUCCESS) {
            pendingProbePort = probe;
            state = State::Faulted;
            return HSA_STATUS_ERROR;
        }
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 181 || stage != (linuxShim ? 2u : 15u)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        status = ensureReady(false);
        if (status != HSA_STATUS_SUCCESS) return status;
        std::array<uint64_t, 3> input{token.token[0], token.token[1], token.size}, output{};
        status = scalar(53, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!output[0] || !output[1] || output[2] != token.size || output[1] > UINT64_MAX - output[2]) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        buffer = {output[0], output[1], output[2]}; return HSA_STATUS_SUCCESS;
    }
    // Device power (power.h). Cached state on the driver side: callable in
    // any session state, without the session lock (a PREPARE can take as
    // long as the driver's quiesce; the port lives as long as this object).
    hsa_status_t powerState(PowerSnapshot &out) override {
        return powerCall(21, amdgpu::power::kQueryTag, out);
    }
    hsa_status_t requestPower(uint64_t op, PowerSnapshot &out) override {
        if (op != amdgpu::power::Prepare && op != amdgpu::power::Resume) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return powerCall(amdgpu::power::kSelector, op, out);
    }

    // Interrupt signals: KFD signal events of this client's KFD process
    // (selectors 86 and 87, dext/sources/session_state.h). Without the
    // session lock: event calls and waits run beside everything else.
    hsa_status_t createSignalEvent(SignalEvent &out, std::string *why) override {
        io_connect_t port;
        {
            std::lock_guard lock(sessionMutex);
            const auto status = eventSupportLocked(why);
            if (status != HSA_STATUS_SUCCESS) return status;
            port = ownerPort;
        }
        uint64_t input[2] = {kEventCreate, 0}, output[kEventWords]{};
        uint32_t raw = 0;
        const auto status = call(port, kSelectorEvent, input, 2, output, kEventWords, &raw);
        if (raw == uint32_t(kIOReturnUnsupported) || raw == uint32_t(kIOReturnBadArgument)) {
            if (why) *why = "the driver does not serve signal events (selector 86)";
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        }
        if (status != HSA_STATUS_SUCCESS) return status;
        const auto error = int64_t(output[0]);
        if (error < 0) {
            if (why) *why = "the driver refused a signal event (errno " + std::to_string(-error) + ")";
            return error == -12 ? HSA_STATUS_ERROR_OUT_OF_RESOURCES :
                   error == -19 ? HSA_STATUS_ERROR_INVALID_ARGUMENT : HSA_STATUS_ERROR;
        }
        if (output[1] > UINT32_MAX || output[2] > UINT32_MAX || !output[3]) return HSA_STATUS_ERROR;
        out = {uint32_t(output[1]), uint32_t(output[2]), output[3]};
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroySignalEvent(uint32_t id) override { return eventCall(kEventDestroy, id); }
    hsa_status_t setSignalEvent(uint32_t id) override { return eventCall(kEventSet, id); }
    hsa_status_t waitSignalEvents(const uint32_t *ids, uint32_t count, uint32_t timeoutMs,
                                  EventWaitResult &result) override {
        if (!ids || !count || count > kEventWaitIDs || !timeoutMs) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        io_connect_t port;
        {
            std::lock_guard lock(sessionMutex);
            port = ownerPort;
        }
        if (!port) return HSA_STATUS_ERROR;
        auto &waiter = eventWaiter();
        if (!waiter.port) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        timeoutMs = std::min(timeoutMs, kEventWaitMaxMs);
        waiter.token = nextEventToken.fetch_add(1, std::memory_order_relaxed);
        waiter.done = false;
        io_user_reference_t reference[kIOAsyncCalloutCount]{};
        reference[kIOAsyncCalloutFuncIndex] = io_user_reference_t(uintptr_t(&eventWaitCompleted));
        reference[kIOAsyncCalloutRefconIndex] = io_user_reference_t(waiter.token);
        uint64_t input[4] = {waiter.token, count, 0, timeoutMs}, output[1]{};
        uint32_t outputs = 1;
        const auto kr = IOConnectCallAsyncMethod(port, kSelectorEventWait,
            IONotificationPortGetMachPort(waiter.port), reference, kIOAsyncCalloutCount,
            input, 4, ids, count * sizeof(uint32_t), output, &outputs, nullptr, nullptr);
        if (kr == kIOReturnNoDevice || kr == kIOReturnNotAttached || kr == MACH_SEND_INVALID_DEST)
            return kDeviceLostStatus;
        if (kr != KERN_SUCCESS || outputs < 1) return HSA_STATUS_ERROR;
        if (int64_t(output[0]) < 0)	// not started: nothing will complete
            return int64_t(output[0]) == -11 || int64_t(output[0]) == -16 ?
                HSA_STATUS_ERROR_OUT_OF_RESOURCES : HSA_STATUS_ERROR;
        // The driver completes the call by its own timeout; this deadline
        // only notices a driver that went away.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs + 2000);
        union {
            mach_msg_header_t header;
            uint8_t bytes[4096];
        } message;
        while (!waiter.done) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return HSA_STATUS_ERROR;
            std::memset(&message.header, 0, sizeof(message.header));
            const auto received = mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                           sizeof(message), IONotificationPortGetMachPort(waiter.port),
                                           mach_msg_timeout_t(left), MACH_PORT_NULL);
            if (received == MACH_RCV_TIMED_OUT) continue;
            if (received != MACH_MSG_SUCCESS) return HSA_STATUS_ERROR;
            IODispatchCalloutFromMessage(nullptr, &message.header, waiter.port);
        }
        if (waiter.status != kIOReturnSuccess || waiter.count < 3 || waiter.args[0] != input[0])
            return HSA_STATUS_ERROR;
        if (int64_t(waiter.args[1]) < 0) return HSA_STATUS_ERROR;	// -EIO: an event went away
        if (waiter.args[2] == 0) { result = EventWaitResult::Fired; return HSA_STATUS_SUCCESS; }
        if (waiter.args[2] == 1) { result = EventWaitResult::TimedOut; return HSA_STATUS_SUCCESS; }
        return HSA_STATUS_ERROR;
    }

private:
    // Selectors 86/87 (session_state.h).
    static constexpr uint32_t kSelectorEvent = 86, kSelectorEventWait = 87;
    static constexpr uint64_t kEventCreate = 0, kEventDestroy = 1, kEventSet = 2;
    static constexpr uint32_t kEventWords = 4, kEventWaitIDs = 64, kEventWaitMaxMs = 1000;
    int eventSupport = -1;	// -1 unknown, 0 no, 1 yes (under sessionMutex)
    std::string eventDecline;
    std::atomic<uint64_t> nextEventToken{1};
    hsa_status_t eventSupportLocked(std::string *why) {
        if (eventSupport < 0) {
            eventSupport = 0;
            if (!linuxShim) {
                eventDecline = "the driver is not the Linux-shim driver";
            } else if (ensureReady() != HSA_STATUS_SUCCESS) {
                eventSupport = -1;
                if (why) *why = "the device is not ready";
                return HSA_STATUS_ERROR;
            } else if (sessionMode != ComputeSessionMode::KFD) {
                eventDecline = "the compute session is on the legacy path (no KFD process)";
            } else {
                std::array<uint64_t, 3> build{};
                if (scalar(43, {}, build) != HSA_STATUS_SUCCESS) {
                    eventDecline = "the driver build could not be read";
                } else if (build[2] < kSignalEventDriverBuild) {
                    eventDecline = "driver build " + std::to_string(build[2]) +
                        " predates signal events (build " + std::to_string(kSignalEventDriverBuild) + ")";
                } else {
                    eventSupport = 1;
                }
            }
        }
        if (eventSupport == 1) return HSA_STATUS_SUCCESS;
        if (why) *why = eventDecline;
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    hsa_status_t eventCall(uint64_t op, uint32_t id) {
        io_connect_t port;
        {
            std::lock_guard lock(sessionMutex);
            if (eventSupport != 1) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            port = ownerPort;
        }
        uint64_t input[2] = {op, id}, output[kEventWords]{};
        const auto status = call(port, kSelectorEvent, input, 2, output, kEventWords);
        if (status != HSA_STATUS_SUCCESS) return status;
        return int64_t(output[0]) < 0 ? HSA_STATUS_ERROR : HSA_STATUS_SUCCESS;
    }
    // One notification port per thread: a thread has one wait in flight.
    // The completion's refcon is the call's token, so a completion that
    // arrives after its call gave up matches nothing.
    struct EventWaiter {
        IONotificationPortRef port = nullptr;
        uint64_t token = 0;
        bool done = false;
        IOReturn status = kIOReturnSuccess;
        uint64_t args[3]{};
        uint32_t count = 0;
        EventWaiter() : port(IONotificationPortCreate(kIOMainPortDefault)) {}
        ~EventWaiter() { if (port) IONotificationPortDestroy(port); }
    };
    static EventWaiter &eventWaiter() {
        thread_local EventWaiter waiter;
        return waiter;
    }
    static void eventWaitCompleted(void *refcon, IOReturn status, void **args, uint32_t count) {
        auto &waiter = eventWaiter();
        if (uint64_t(uintptr_t(refcon)) != waiter.token) return;	// a stale completion
        waiter.status = status;
        waiter.count = std::min<uint32_t>(count, 3);
        for (uint32_t i = 0; i < waiter.count; ++i) waiter.args[i] = uint64_t(uintptr_t(args[i]));
        waiter.done = true;
    }
    hsa_status_t powerCall(uint32_t selector, uint64_t input, PowerSnapshot &out) {
        io_connect_t port;
        {
            std::lock_guard lock(sessionMutex);
            port = ownerPort;
        }
        if (!port || !linuxShim) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        PowerSnapshot snapshot;
        uint32_t raw = 0, count = 0;
        const auto status = call(port, selector, &input, 1, snapshot.words.data(),
                                 uint32_t(snapshot.words.size()), &raw, &count);
        // Drivers before the protocol: an unknown tag or selector.
        if (raw == uint32_t(kIOReturnUnsupported) || raw == uint32_t(kIOReturnBadArgument) ||
            raw == uint32_t(kIOReturnNotPermitted))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (status == kDeviceLostStatus) {
            // The driver no longer answers: the device left the bus (or the
            // driver stopped). Report the loss the driver would have.
            namespace power = amdgpu::power;
            snapshot = {};
            snapshot.words[power::Version] = power::kVersion;
            snapshot.words[power::State] = uint64_t(power::PowerState::Lost);
            snapshot.words[power::Generation] = lostGeneration;
            snapshot.words[power::Flags] = power::LinkDown;
            snapshot.words[power::Cause] = power::kCauseDeviceRemoved;
            snapshot.words[power::Error] = uint64_t(int64_t(-19)); // ENODEV
            out = snapshot;
            return HSA_STATUS_SUCCESS;
        }
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!snapshot.valid()) return HSA_STATUS_ERROR;
        // A later loss the runtime reports itself is a new generation.
        lostGeneration = snapshot.generation() + 1;
        out = snapshot;
        return HSA_STATUS_SUCCESS;
    }
    std::atomic<uint64_t> lostGeneration{1};
    const bool linuxShim;
    enum class State { Unclaimed, Initializing, Ready, Faulted } state = State::Unclaimed;
    // Selector 59's outputs (2, or 4 from kQueueFaultDriverBuild on; 0 until
    // asked), and the GPU memory fault of this connection's KFD process.
    // Whether the driver serves CodeSync (asked once, kCodeSyncDriverBuild).
    bool codeSyncSelectorKnown = false, codeSyncSelector = false;
    size_t serviceOutputs = 0;
    bool faulted = false;
    MemoryFault fault;
    // Queues the CP stopped with an error code, since when (serviceQueueLocked).
    static constexpr auto kQueueErrorSettle = std::chrono::milliseconds(250);
    std::map<uint64_t,std::chrono::steady_clock::time_point> queueErrorSince;
    std::mutex sessionMutex;
    std::mutex codeSyncMutex;
    DeviceBuffer codeSyncBuffer;
    bool codeSyncUploaded = false;
    std::map<uint64_t,std::array<uint64_t,2>> hardwareQueues;
    io_connect_t ownerPort = IO_OBJECT_NULL;
    io_connect_t pendingProbePort = IO_OBJECT_NULL;
    uint64_t lastComputeFence = 0;
    uint64_t capacity = 0;
    DeviceBuffer staging;
    SharedBuffer mappedStaging;
    bool mappedStagingDeclined = false;
    static constexpr size_t kMappedStagingBytes = 4u << 20;
    uint64_t hostWindowBase = 0, hostWindowSize = 0;
    // The window's free VA, held for the session (host_window.h), and the
    // bytes of shared buffers currently mapped in it.
    HostWindowReservation hostReservation;
    uint64_t sharedBytes = 0;
    // Device-local buffers this connection holds, and the IOReturn of the
    // last RPC: what an allocation failure report states.
    uint64_t vramBytes = 0, vramCount = 0;
    AllocationCensus census;
    uint32_t lastIOReturn = 0;
    std::chrono::steady_clock::time_point lastFailureReport{};
    // An allocation the driver refused: what was asked, what the driver
    // answered, what the device's memory manager holds, what this process
    // holds, and the end of the driver's log, which names the refusal. At
    // most one report every few seconds; a refusal is often retried.
    void reportAllocationFailureLocked(const char *kind, uint64_t bytes, hsa_status_t status) {
        const auto raw = lastIOReturn;
        const auto now = std::chrono::steady_clock::now();
        if (lastFailureReport.time_since_epoch().count() && now - lastFailureReport < std::chrono::seconds(5)) return;
        lastFailureReport = now;
        std::fprintf(stderr, "mac_linuxgpu: %s allocation of %llu bytes refused: IOReturn %#x (HSA status %#x); "
            "this process holds %llu VRAM buffers (%llu bytes) and %zu shared buffers (%llu bytes)\n",
            kind, (unsigned long long)bytes, raw, unsigned(status), (unsigned long long)vramCount,
            (unsigned long long)vramBytes, sharedBuffers.size(), (unsigned long long)sharedBytes);
        std::fputs(census.report().c_str(), stderr);
        if (state != State::Ready || !linuxShim) return;
        std::array<uint64_t, 6> usage{};
        const uint64_t tag = 9;
        if (call(ownerPort, 21, &tag, 1, usage.data(), uint32_t(usage.size())) == HSA_STATUS_SUCCESS)
            std::fprintf(stderr, "mac_linuxgpu: device VRAM total %llu usable %llu used %llu free %llu; "
                "CPU-visible %llu used %llu\n", (unsigned long long)usage[0], (unsigned long long)usage[1],
                (unsigned long long)usage[2], (unsigned long long)usage[3], (unsigned long long)usage[4],
                (unsigned long long)usage[5]);
        dumpKernelLogTailLocked(4096);
    }
    // The last `bytes` of the driver's cached kernel log.
    void dumpKernelLogTailLocked(uint64_t bytes) {
        uint64_t cursor = 0, end = 0;
        bool header = false;
        for (unsigned chunk = 0; chunk < 64; ++chunk) {
            const uint64_t input[] = {kCachedKernelLog, cursor};
            std::array<uint64_t, 16> output{};
            uint32_t count = output.size();
            if (rpcMethod(ownerPort, 21, input, 2, nullptr, 0, output.data(), &count, nullptr, nullptr) != KERN_SUCCESS ||
                count < 3 || output[2] > (count - 3) * sizeof(uint64_t))
                return;
            if (chunk == 0) {
                end = output[0];
                const uint64_t from = end > bytes ? end - bytes : 0;
                if (from > cursor) { cursor = from; continue; }
            }
            const uint64_t next = output[1], length = output[2];
            char data[104];
            for (uint64_t i = 0; i < length; ++i) data[i] = char(output[3 + i / 8] >> ((i % 8) * 8));
            if (!header) { std::fputs("mac_linuxgpu: driver log tail:\n", stderr); header = true; }
            std::fwrite(data, 1, size_t(length), stderr);
            if (!length || next >= end) break;
            cursor = next;
        }
        std::fputc('\n', stderr);
    }
    void reserveHostWindowLocked() {
        const auto reserved = hostReservation.reserve(hostWindowBase, hostWindowSize);
        if (const char *trace = std::getenv("MAC_HSA_SESSION_TRACE"); trace && trace[0] == '1')
            std::fprintf(stderr, "mac_linuxgpu: host window %#llx+%#llx: %#llx bytes held free for shared buffers\n",
                (unsigned long long)hostWindowBase, (unsigned long long)hostWindowSize, (unsigned long long)reserved);
    }
    uint64_t sharedCapacityLocked() const {
        const auto budget = hostMemoryBudget();
        return budget ? std::min(hostWindowSize, budget) : hostWindowSize;
    }
    ComputeSessionMode sessionMode = ComputeSessionMode::Unknown;
    std::map<uint64_t, SharedBuffer> sharedBuffers;
    std::map<std::string, std::vector<uint8_t>> firmware;
    // Linux-shim on-demand firmware servicer; non-null only while InitDevice
    // (selector 9) is in flight on ownerPort.
    struct mlg_fw_service *firmwareService = nullptr;
    struct {
        bool present = false;
        uint32_t selector = 0, result = 0, count = 0, expected = 0;
    } initializationTransportFailure;
    // Static device descriptions, read once per ready session. A driver that
    // declines a tag leaves it unreported for the session (nullptr).
    std::array<uint64_t, kTopologyQueryWords> topology{};
    std::array<uint64_t, kProductNameQueryWords> productName{};
    bool topologyQueried = false, topologyReported = false;
    bool productNameQueried = false, productNameReported = false;
    bool unknownTargetReported = false;
    bool rsrc1ClampAndIEEE = false; // from the last successful read()
    // Queue slots usable by this session: the Linux-shim driver's reported
    // count (one when it predates the report). The converted protocol reports
    // exhaustion itself, so it has no client-side limit.
    uint32_t queueSlotLimit = UINT32_MAX;
    void noteIsa(const DeviceSnapshot &snapshot) {
        IsaTarget isa;
        rsrc1ClampAndIEEE = deviceIsa(snapshot, isa) && hasRsrc1ClampAndIEEE(isa);
        if (linuxShim) queueSlotLimit = deviceQueueSlots(snapshot);
    }
    bool queueSlotsExhaustedLocked() const { return hardwareQueues.size() >= queueSlotLimit; }
    const uint64_t *topologyLocked() {
        if (!topologyQueried && state == State::Ready) {
            topologyQueried = true;
            const uint64_t tag = kTopologyQueryTag;
            topologyReported = scalar(21, {&tag, 1}, topology) == HSA_STATUS_SUCCESS &&
                topology[TopologyVersion] >= 1;
        }
        return topologyReported ? topology.data() : nullptr;
    }
    const uint64_t *productNameLocked() {
        if (!productNameQueried && state == State::Ready) {
            productNameQueried = true;
            const uint64_t tag = kProductNameQueryTag;
            productNameReported = scalar(21, {&tag, 1}, productName) == HSA_STATUS_SUCCESS;
        }
        return productNameReported ? productName.data() : nullptr;
    }

    hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input,
                        std::span<uint64_t> output) override {
        uint32_t raw = 0, count = 0;
        const auto status = call(ownerPort, selector, input.data(), uint32_t(input.size()),
                    output.data(), uint32_t(output.size()), &raw, &count);
        lastIOReturn = raw;
        if (linuxShim && state == State::Initializing && status != HSA_STATUS_SUCCESS &&
            !initializationTransportFailure.present)
            initializationTransportFailure = {true, selector, raw, count, uint32_t(output.size())};
        return status;
    }
    // Converted protocol only (initializeDevice). The Linux-shim driver
    // requests its own firmware through the on-demand servicer instead.
    hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) override {
        if (linuxShim) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto configured = std::getenv("MAC_LINUXGPU_FIRMWARE_DIR");
        const std::string directory = configured ? configured :
            "/Applications/MacLinuxGPUHost.app/Contents/Resources/firmware";
        for (const auto &file : files) {
            std::ifstream stream(directory + "/" + file.name, std::ios::binary | std::ios::ate);
            if (!stream) return HSA_STATUS_ERROR_INVALID_FILE;
            const auto size = stream.tellg();
            if (size <= 0 || size > (32 << 20)) return HSA_STATUS_ERROR_INVALID_FILE;
            auto &data = firmware[file.name];
            data.resize(size_t(size));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char *>(data.data()), size)) return HSA_STATUS_ERROR_INVALID_FILE;
        }
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t uploadFirmware(const FirmwareFile &file) override {
        if (linuxShim) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found = firmware.find(file.name);
        if (found == firmware.end()) return HSA_STATUS_ERROR_INVALID_FILE;
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(ownerPort, 6, mach_task_self(), &address, &size, kIOMapAnywhere) != KERN_SUCCESS)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        const auto &data = found->second;
        hsa_status_t status = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        if (address && data.size() <= size) {
            std::memcpy(reinterpret_cast<void *>(address), data.data(), data.size());
            const std::array<uint64_t, 2> input{file.type, data.size()};
            uint64_t output = 0;
            status = scalar(10, input, {&output, 1});
        }
        if (IOConnectUnmapMemory64(ownerPort, 6, mach_task_self(), address) != KERN_SUCCESS)
            return HSA_STATUS_ERROR;
        return status;
    }
    void waitAfterReset() override { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
    // Serves <firmware root>/<name> (MAC_LINUXGPU_FIRMWARE_ROOT, else the
    // installed root) through the mailbox the driver exports on ownerPort.
    // A driver that exports none, or any other failure, leaves the driver
    // with its embedded firmware only; that is reported once per process.
    // Empty selects the servicer's default ($MAC_LINUXGPU_FIRMWARE_ROOT, else
    // the installed root). An iOS app cannot read outside its container, so
    // there the default is the Firmware directory inside the app bundle.
    static std::string firmwareRoot() {
#if TARGET_OS_IOS
        if (const char *configured = std::getenv("MAC_LINUXGPU_FIRMWARE_ROOT"); configured && *configured) return {};
        std::string root;
        if (const auto bundle = CFBundleGetMainBundle()) {
            if (const auto url = CFBundleCopyResourcesDirectoryURL(bundle)) {
                char path[PATH_MAX];
                if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(path), sizeof(path)))
                    root = std::string(path) + "/Firmware";
                CFRelease(url);
            }
        }
        return root;
#else
        return {};
#endif
    }
    bool startFirmwareService() override {
        if (firmwareService) return true;
        const std::string root = firmwareRoot();
        const int error = mlg_fw_service_start_connection(ownerPort, root.empty() ? nullptr : root.c_str(),
                                                          &firmwareService);
        if (!error && firmwareService) return true;
        firmwareService = nullptr;
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set())
            std::fprintf(stderr, "mac_linuxgpu: firmware servicer unavailable (%s); "
                         "device initialization can use only the driver's embedded firmware\n",
                         error == -EOPNOTSUPP ? "the driver exports no firmware mailbox" :
                         std::strerror(error < 0 ? -error : EIO));
        return false;
    }
    void stopFirmwareService() override {
        mlg_fw_service_stop(firmwareService);
        firmwareService = nullptr;
    }
    hsa_status_t reserveHostWindow(uint64_t bytes,uint64_t &base) override {
        mach_vm_address_t address=0;
        const auto r=mach_vm_map(mach_task_self(),&address,bytes,bytes-1,
            VM_FLAGS_ANYWHERE,MACH_PORT_NULL,0,false,VM_PROT_NONE,VM_PROT_NONE,VM_INHERIT_NONE);
        base=address;
        return r==KERN_SUCCESS ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    hsa_status_t releaseHostWindow(uint64_t base,uint64_t bytes) override {
        return mach_vm_deallocate(mach_task_self(),base,bytes)==KERN_SUCCESS ?
            HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    }
    hsa_status_t ensureReady(bool allowInitialize = true) {
        if (state == State::Ready) return HSA_STATUS_SUCCESS;
        if (state != State::Unclaimed) return HSA_STATUS_ERROR;
        if (const auto opened = openSessionClient(service, ownerPort); opened != HSA_STATUS_SUCCESS)
            return opened;
        state = State::Initializing;
        if (linuxShim) {
            initializationTransportFailure = {};
            ShimInitializationResult result;
            ShimInitializationFailure failure;
            bool claimed=false;
            const auto status=initializeShimDevice(*this,result,claimed,allowInitialize,&failure);
            if (status==HSA_STATUS_SUCCESS) {
                capacity=result.capacity;
                hostWindowBase=result.hostBase;
                hostWindowSize=result.hostBytes;
                sessionMode=result.sessionMode;
                state=State::Ready;faulted=false;fault={};serviceOutputs=0;
                reserveHostWindowLocked();
                if (const char *trace=std::getenv("MAC_HSA_SESSION_TRACE"); trace && trace[0]=='1')
                    std::fprintf(stderr, "mac_linuxgpu: compute session %s, host window %#llx+%#llx\n",
                        sessionMode==ComputeSessionMode::KFD ? "KFD process" :
                        sessionMode==ComputeSessionMode::Legacy ? "legacy HQDs" : "unreported",
                        (unsigned long long)hostWindowBase, (unsigned long long)hostWindowSize);
                return status;
            }
            const auto emit = [](std::string_view text) {
                (void)std::fwrite(text.data(), 1, text.size(), stderr);
            };
            reportShimInitializationFailure(failure, status, emit);
            if (initializationTransportFailure.present) {
                const auto &detail = initializationTransportFailure;
                std::fprintf(stderr,
                    "mac_linuxgpu: first failed initialization RPC: selector=%u IOReturn=%#x output_count=%u expected=%u\n",
                    detail.selector, detail.result, detail.count, detail.expected);
            }
            // Preserve cached evidence through this existing owner before its
            // Close can finish teardown. No diagnostic claims PCI or retries init.
            if (ownerPort) dumpCachedShimDiagnostics(
                [this](const uint64_t *input, uint32_t inputs, uint64_t *output, uint32_t *outputs) {
                    return uint32_t(rpcMethod(ownerPort, 21, input, inputs, nullptr, 0, output, outputs,
                                              nullptr, nullptr));
                }, emit);
            (void)std::fflush(stderr);
            const auto closed=closeConnection(ownerPort);
            if (closed==KERN_SUCCESS) ownerPort=IO_OBJECT_NULL;
            // A failed claimed session must not automatically retry probe on
            // the next property query. Retain a failed-close port for teardown.
            state=claimed || closed!=KERN_SUCCESS ? State::Faulted : State::Unclaimed;
            return closed==KERN_SUCCESS ? status : HSA_STATUS_ERROR;
        }
        bool claimed = false;
        hsa_status_t status;
        try { status = initializeDevice(*this, claimed, capacity, allowInitialize); }
        catch (const std::bad_alloc &) { status = HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
        firmware.clear();
        if (!claimed) {
            // Busy acquisition must not leave an idle client blocking Host Stop.
            closeConnection(ownerPort);
            ownerPort = IO_OBJECT_NULL;
            state = State::Unclaimed;
            return status;
        }
        state = status == HSA_STATUS_SUCCESS ? State::Ready : State::Faulted;
        return status;
    }
    hsa_status_t ensureHostWindow() {
        if (hostWindowBase) return HSA_STATUS_SUCCESS;
        std::array<uint64_t, 3> build{};
        auto status = scalar(43, {}, build);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (build[2] < 182) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        uint64_t candidate = 0;
        std::array<uint64_t, 3> window{};
        status = scalar(54, {&candidate, 1}, window);
        if (status != HSA_STATUS_SUCCESS) return status;
        // The driver's aperture size: a power of two below the user VA limit.
        if (window[1] < 16384 || window[1] > (1ull << 45) || (window[1] & (window[1] - 1))) return HSA_STATUS_ERROR;
        if (!window[0]) {
            // Ask Mach for an unused aligned range. It is only a placement hint;
            // each later IOKit mapping must independently succeed at its GPU VA.
            mach_vm_address_t reservation = 0;
            const auto result = mach_vm_map(mach_task_self(), &reservation, window[1], window[1] - 1,
                VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
            if (result != KERN_SUCCESS) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            candidate = reservation;
            const auto reservedSize = window[1];
            status = scalar(54, {&candidate, 1}, window);
            const auto released = mach_vm_deallocate(mach_task_self(), reservation, reservedSize);
            if (released != KERN_SUCCESS) return HSA_STATUS_ERROR;
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        if (window[0] < (1ull << 32) || (window[0] & (window[1] - 1)) ||
            window[0] >= (1ull << 47) || window[1] > (1ull << 47) - window[0]) return HSA_STATUS_ERROR;
        if (!window[2]) {
            const uint64_t seed = arc4random();
            std::array<uint64_t, 6> result{};
            status = scalar(44, {&seed, 1}, result);
            if (status != HSA_STATUS_SUCCESS || result[0] || result[1] != 8 || result[2]) {
                state = State::Faulted; return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
            }
        }
        hostWindowBase = window[0]; hostWindowSize = window[1];
        reserveHostWindowLocked();
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateSharedBufferLocked(uint64_t bytes, SharedBuffer &out) {
        out = {};
        auto status = ensureReady();
        if (status != HSA_STATUS_SUCCESS) return status;
        status = ensureHostWindow();
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!bytes || bytes > hostWindowSize || bytes > UINT64_MAX - 16383)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        const uint64_t rounded = (bytes + 16383) & ~uint64_t(16383);
        if (const auto capacity = sharedCapacityLocked(); rounded > capacity - std::min(capacity, sharedBytes)) {
            std::fprintf(stderr, "mac_linuxgpu: shared host memory budget exhausted: %#llx bytes requested, "
                "%#llx of %#llx in use (MAC_HSA_HOST_MEMORY_BUDGET / mac_hsa_set_host_memory_budget)\n",
                (unsigned long long)rounded, (unsigned long long)sharedBytes, (unsigned long long)capacity);
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        SharedBuffer buffer;
        status = allocateRaw(rounded, 2, buffer.device);
        if (status != HSA_STATUS_SUCCESS) { reportAllocationFailureLocked("shared (GTT)", rounded, status); return status; }
        std::array<uint64_t, 2> mapping{};
        status = scalar(36, {&buffer.device.handle, 1}, mapping);
        mach_vm_address_t address = buffer.device.address;
        mach_vm_size_t size = 0;
        kern_return_t mapped = KERN_INVALID_ARGUMENT;
        bool taken = false;
        if (status == HSA_STATUS_SUCCESS && mapping[0] <= UINT32_MAX && mapping[1] == buffer.device.size &&
            address >= hostWindowBase && address - hostWindowBase <= hostWindowSize &&
            buffer.device.size <= hostWindowSize - (address - hostWindowBase)) {
            buffer.memoryType = uint32_t(mapping[0]);
            // The window's VA is held for exactly this: release this buffer's
            // range just before the placed mapping. A placed mapping still
            // fails on a collision (a range the reservation never held); it
            // never overwrites process memory.
            taken = hostReservation.take(buffer.device.address, buffer.device.size);
            mapped = IOConnectMapMemory64(ownerPort, buffer.memoryType, mach_task_self(), &address, &size, 0);
            if (mapped == KERN_SUCCESS) {
                if (address == buffer.device.address && size == buffer.device.size) {
                    buffer.host = reinterpret_cast<void *>(address);
                    try {
                        sharedBuffers.emplace(buffer.device.handle, buffer);
                        sharedBytes += buffer.device.size;
                        census.add(buffer.device.handle, AllocationCensus::Kind::Shared, buffer.device.size);
                        std::memset(buffer.host, 0, size);
                        std::atomic_thread_fence(std::memory_order_seq_cst);
                        out = buffer; return HSA_STATUS_SUCCESS;
                    } catch (const std::bad_alloc &) { /* unmap before releasing backing */ }
                }
                if (IOConnectUnmapMemory64(ownerPort, buffer.memoryType, mach_task_self(), address) != KERN_SUCCESS) {
                    state = State::Faulted; return HSA_STATUS_ERROR;
                }
            }
            if (taken) hostReservation.give(buffer.device.address, buffer.device.size);
        }
        if (status == HSA_STATUS_SUCCESS)
            std::fprintf(stderr, "mac_linuxgpu: shared buffer %#llx+%#llx did not map at its GPU VA "
                "(IOReturn %#x)%s\n", (unsigned long long)buffer.device.address,
                (unsigned long long)buffer.device.size, unsigned(mapped),
                taken ? "" : "; something else in the process maps that range");
        const auto cleanup = scalar(17, {&buffer.device.handle, 1}, {});
        if (cleanup != HSA_STATUS_SUCCESS) { state = State::Faulted; return cleanup; }
        return status != HSA_STATUS_SUCCESS ? status : HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    hsa_status_t allocateRaw(uint64_t bytes, uint64_t domain, DeviceBuffer &buffer) {
        const std::array<uint64_t, 4> input{bytes, domain, 16384, 0};
        std::array<uint64_t, 3> output{};
        const auto status = scalar(16, input, output);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (!output[0] || !output[1] || (domain != 2 && output[2]) || output[1] > UINT64_MAX - bytes) {
            state = State::Faulted; return HSA_STATUS_ERROR;
        }
        buffer = {output[0], output[1], bytes};
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t copyRaw(uint64_t src, uint64_t srcOffset, uint64_t dst, uint64_t dstOffset, size_t size) {
        const std::array<uint64_t, 5> input{src, srcOffset, dst, dstOffset, size};
        uint64_t operation = UINT64_MAX;
        const auto status = scalar(48, input, {&operation, 1});
        if (status == HSA_STATUS_SUCCESS && !operation) return status;
        if (status == kDeviceSuspendedStatus) return status; // refused before submission
        // A failed submission may still reference staging. Never free/reuse it.
        state = State::Faulted;
        return status == HSA_STATUS_SUCCESS ? HSA_STATUS_ERROR : status;
    }
    hsa_status_t transfer(const DeviceBuffer &buffer, uint64_t offset, void *host, size_t bytes, bool upload) {
        if (state != State::Ready) return HSA_STATUS_ERROR;
        if (!host || offset > buffer.size || bytes > buffer.size - offset)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!bytes) return HSA_STATUS_SUCCESS;
        if (!mappedStaging.device.handle && !mappedStagingDeclined) {
            const auto status = allocateSharedBufferLocked(kMappedStagingBytes, mappedStaging);
            if (status != HSA_STATUS_SUCCESS) {
                if (state != State::Ready ||
                    (status != HSA_STATUS_ERROR_OUT_OF_RESOURCES &&
                     status != HSA_STATUS_ERROR_INVALID_ALLOCATION &&
                     status != HSA_STATUS_ERROR_INVALID_ARGUMENT))
                    return status;
                // Older drivers and clean mapping declines retain the RPC path.
                mappedStagingDeclined = true;
            }
        }
        const bool mapped = mappedStaging.device.handle != 0;
        if (!mapped && !staging.handle) {
            const auto status = allocateRaw(16384, 1, staging);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        auto pointer = static_cast<uint8_t *>(host);
        std::array<uint8_t, 4096> rpcChunk{};
        auto *chunk = mapped ? static_cast<uint8_t *>(mappedStaging.host) : rpcChunk.data();
        const auto chunkBytes = mapped ? kMappedStagingBytes : rpcChunk.size();
        const auto stagingHandle = mapped ? mappedStaging.device.handle : staging.handle;
        while (bytes) {
            const auto aligned = offset & ~uint64_t(3);
            const auto prefix = size_t(offset - aligned);
            const auto length = std::min(bytes, chunkBytes - prefix);
            const auto span = (prefix + length + 3) & ~size_t(3);
            hsa_status_t status = HSA_STATUS_SUCCESS;
            const std::array<uint64_t, 3> io{stagingHandle, 0, span};
            // A partial dword write preserves neighboring bytes using read/modify/write.
            if (!upload || prefix || length != span) {
                if (mapped) std::atomic_thread_fence(std::memory_order_seq_cst);
                status = copyRaw(buffer.handle, aligned, stagingHandle, 0, span);
                if (status != HSA_STATUS_SUCCESS) return status;
                if (mapped) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                } else {
                    size_t returned = span;
                    const auto result = rpcMethod(ownerPort, 50, io.data(), 3, nullptr, 0,
                                                            nullptr, nullptr, chunk, &returned);
                    if (result == kIOReturnOffline) return kDeviceSuspendedStatus;
                    if (result != KERN_SUCCESS || returned != span) { state = State::Faulted; return HSA_STATUS_ERROR; }
                }
            }
            if (upload) {
                std::memcpy(chunk + prefix, pointer, length);
                if (mapped) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                } else {
                    const auto result = rpcMethod(ownerPort, 49, io.data(), 3, chunk, span,
                                                            nullptr, nullptr, nullptr, nullptr);
                    if (result == kIOReturnOffline) return kDeviceSuspendedStatus;
                    if (result != KERN_SUCCESS) { state = State::Faulted; return HSA_STATUS_ERROR; }
                }
                status = copyRaw(stagingHandle, 0, buffer.handle, aligned, span);
                if (status != HSA_STATUS_SUCCESS) return status;
                if (mapped) std::atomic_thread_fence(std::memory_order_seq_cst);
            } else std::memcpy(pointer, chunk + prefix, length);
            offset += length; pointer += length; bytes -= length;
        }
        return HSA_STATUS_SUCCESS;
    }

public:
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        std::lock_guard lock(sessionMutex);
        if (linuxShim) {
            // hsa_init joins the shim's upstream probe through selector 9 and
            // retains this same UserClient for all subsequent allocations and
            // queues. The port closes with the connection on hsa_shut_down.
            auto status = ensureReady();
            if (status != HSA_STATUS_SUCCESS) return status;
            std::array<uint64_t, 3> identity{}, gfx{};
            std::array<uint64_t, 2> vram{};
            uint64_t tag = 1, stage = 0;
            status = scalar(43, {}, identity);
            if (status == HSA_STATUS_SUCCESS) status = scalar(21, {&tag, 1}, gfx);
            tag = 2;
            if (status == HSA_STATUS_SUCCESS) status = scalar(21, {&tag, 1}, vram);
            tag = 4;
            if (status == HSA_STATUS_SUCCESS) status = scalar(21, {&tag, 1}, {&stage, 1});
            if (status != HSA_STATUS_SUCCESS) return status;
            if (identity[0] != 0x414d444750554142ull || identity[1] != 1 ||
                identity[2] < kQueueResourceDriverBuild || stage != 2 ||
                !gfx[0] || gfx[0] > UINT8_MAX || gfx[1] > UINT8_MAX || gfx[2] > UINT8_MAX ||
                !vram[0] || vram[0] > vram[1])
                return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
            snapshot = {registryID, identity[2], stage, vram[0], vram[1],
                        uint32_t(gfx[0]), uint32_t(gfx[1]), uint32_t(gfx[2])};
            snapshot.originalAtomicCaps = originalAtomicCaps;
            snapshot.sessionMode = sessionMode;
            applyDeviceTopology(snapshot, topologyLocked());
            IsaTarget isa;
            if (!deviceIsa(snapshot, isa)) {
                // ROCr likewise skips a GPU node whose ISA it does not know.
                if (!unknownTargetReported)
                    std::fprintf(stderr, "mac_linuxgpu: GPU with GC IP %u.%u.%u (gfx_target_version %u) "
                                 "names no known ISA; agent not published\n", snapshot.gfxMajor,
                                 snapshot.gfxMinor, snapshot.gfxRevision, snapshot.gfxTargetVersion);
                unknownTargetReported = true;
                return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
            }
            noteIsa(snapshot);
            return HSA_STATUS_SUCCESS;
        }
        // A registry reference does not keep a user client connected. Each probe
        // closes before returning, so idle discovery cannot block owner shutdown.
        io_connect_t port = ownerPort;
        if (!port) {
            if (const auto opened = openSessionClient(service, port); opened != HSA_STATUS_SUCCESS)
                return opened;
        }
        struct Close { io_connect_t port; ~Close() { if (port) closeConnection(port); } } close{ownerPort ? 0 : port};
        uint64_t identity[3]{};
        auto status = call(port, 43, nullptr, 0, identity, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (identity[0] != 0x414d444750554142ull || identity[1] != 1 || identity[2] < 172)
            return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
        uint64_t gfx[3]{}, vram[2]{}, stage = 0, tag = 1;
        status = call(port, 21, &tag, 1, gfx, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 2;
        status = call(port, 21, &tag, 1, vram, 2);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 4;
        status = call(port, 21, &tag, 1, &stage, 1);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (gfx[0] > UINT32_MAX || gfx[1] > UINT32_MAX || gfx[2] > UINT32_MAX ||
            vram[0] > vram[1]) return HSA_STATUS_ERROR;
        snapshot = {registryID, identity[2], stage, vram[0], vram[1],
                    uint32_t(gfx[0]), uint32_t(gfx[1]), uint32_t(gfx[2])};
        snapshot.originalAtomicCaps=originalAtomicCaps;
        // The converted protocol's tag numbering predates the topology tag.
        applyDeviceTopology(snapshot, nullptr);
        noteIsa(snapshot);
        return HSA_STATUS_SUCCESS;
    }
};

} // namespace

hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    // Host unit-testing: the fake in-memory backend stands in for the IOKit
    // transport when MAC_LINUXGPU_FAKE_TRANSPORT=1. The runtime's logic is
    // transport-agnostic (it calls only Connection virtual methods), so the
    // same code paths run against either backend.
    if (const auto fake = std::getenv("MAC_LINUXGPU_FAKE_TRANSPORT"); fake && fake[0] == '1')
        return discover_fake(connections);
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto matching = IOServiceMatching("IOUserService");
    if (!matching) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return HSA_STATUS_ERROR;
    IOObject iteratorOwner(iterator);
    try {
        for (io_service_t service; (service = IOIteratorNext(iterator));) {
            IOObject serviceOwner(service);
            const auto bundle = IORegistryEntryCreateCFProperty(
                service, CFSTR("CFBundleIdentifier"), kCFAllocatorDefault, 0);
            const auto userClass = IORegistryEntryCreateCFProperty(
                service, CFSTR("IOUserClass"), kCFAllocatorDefault, 0);
            char bundleName[128]{}, className[128]{};
            const bool metadataValid = bundle && userClass &&
                CFGetTypeID(bundle) == CFStringGetTypeID() &&
                CFGetTypeID(userClass) == CFStringGetTypeID() &&
                CFStringGetCString(static_cast<CFStringRef>(bundle), bundleName,
                                   sizeof(bundleName), kCFStringEncodingUTF8) &&
                CFStringGetCString(static_cast<CFStringRef>(userClass), className,
                                   sizeof(className), kCFStringEncodingUTF8);
            if (bundle) CFRelease(bundle);
            if (userClass) CFRelease(userClass);
            auto protocol = metadataValid ? driverProtocol(bundleName, className)
                                          : DriverProtocol::Unknown;
            // An iOS app's sandbox hides the registry properties of an
            // embedded driver's service, but its registry name stays
            // readable. The RuntimeBuild handshake still proves identity.
            if (!metadataValid) {
                io_name_t registryName{};
                if (IORegistryEntryGetName(service, registryName) == KERN_SUCCESS &&
                    std::string_view(registryName) == "MacLinuxGPU")
                    protocol = DriverProtocol::LinuxShim;
            }
            if (protocol == DriverProtocol::Unknown) continue;
            uint64_t registryID = 0;
            if (IORegistryEntryGetRegistryEntryID(service, &registryID) != KERN_SUCCESS)
                return HSA_STATUS_ERROR;
            auto connection = std::make_shared<IOKitConnection>(
                captureOriginalAtomicCaps(service), protocol == DriverProtocol::LinuxShim);
            IOObjectRetain(service);
            connection->service = service;
            connection->registryID = registryID;
            DeviceSnapshot snapshot;
            const auto status = connection->read(snapshot);
            if (status == HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS) continue;
            if (status != HSA_STATUS_SUCCESS) return status;
            connections.push_back(std::move(connection));
        }
    } catch (const std::bad_alloc &) {
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    return HSA_STATUS_SUCCESS;
}
} // namespace mac_hsa
