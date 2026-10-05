/* On-demand firmware during Linux-shim initialization, end to end in one
 * process: the production IOKit transport (transport_iokit.cpp) drives
 * InitDevice, the production client servicer (host/fw_mailbox_service.c and
 * host/fw_mailbox_iokit.c) maps the mailbox through the owner connection, and
 * the production dext side (linuxu/src/fw/fw_mailbox.c) requests firmware
 * from a separate "dext" thread while selector 9 blocks the caller, exactly
 * as upstream's probe does on the driver's serial queue. Only the OS
 * boundaries (service open/close, scalar RPCs, memory mapping, host VA
 * reservation) are replaced; nothing touches a real service or device.
 *
 * Firmware names are synthetic: the servicer serves whatever is asked for. */
#define MAC_HSA_IDLE_DIAGNOSTIC
#define IOServiceOpen test_service_open
#define IOServiceClose test_service_close
#define IOConnectCallScalarMethod test_scalar
#define IOConnectMapMemory64 test_map_memory
#define IOConnectUnmapMemory64 test_unmap_memory
#define mach_vm_map test_vm_map
#define mach_vm_deallocate test_vm_deallocate
/* The IOKit calls are replaced: every selector reaches them synchronously. */
#define MLG_SELECTOR_CALL_TEST_SYNC
#include "../src/transport_iokit.cpp"
#include "fw/fw_mailbox.h"
#include "../../linuxu/headers/rt/fw_mailbox.h"
#include <cassert>
#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr io_connect_t test_port = 991;
constexpr uint64_t window_bytes = 512ull << 20, window_base = 0x800000000ull;
/* A data window smaller than the image, so the transfer is chunked. */
constexpr size_t mailbox_bytes = MLG_FW_MAILBOX_HEADER_SIZE + 16384;
constexpr const char *present_name = "amdgpu/synthetic_ip_9_8_7.bin";
constexpr const char *absent_name = "amdgpu/synthetic_absent_9_8_7.bin";

uint8_t *mailbox_region;
bool mapping_supported = true, ready, port_open;
unsigned maps, unmaps, init_calls, opens, closes;
std::vector<uint8_t> image;
struct DextResult { int present = 1, absent = 1, escaped = 1; size_t size = 0; bool equal = false; } dext;

void write_image(const std::string &root) {
    image.resize(100000);
    for (size_t i = 0; i < image.size(); ++i) image[i] = uint8_t(i * 13 + 5);
    assert(mkdir((root + "/amdgpu").c_str(), 0755) == 0);
    FILE *file = std::fopen((root + "/" + present_name).c_str(), "wb");
    assert(file && std::fwrite(image.data(), 1, image.size(), file) == image.size());
    std::fclose(file);
}

/* What upstream does inside amdgpu_device_init: request_firmware() from the
 * dext's own thread, which polls the mailbox until the servicer answers. */
void dext_probe_requests_firmware() {
    uint8_t *blob = nullptr;
    size_t size = 0;
    dext.present = fw_mailbox_fetch(present_name, &blob, &size);
    dext.size = size;
    dext.equal = blob && size == image.size() && !std::memcmp(blob, image.data(), size);
    std::free(blob);
    blob = nullptr;
    dext.absent = fw_mailbox_fetch(absent_name, &blob, &size);
    assert(!blob);
    dext.escaped = fw_mailbox_fetch("amdgpu/../../etc/passwd", &blob, &size);
    assert(!blob);
}
}

extern "C" kern_return_t test_service_open(io_service_t service, task_port_t, uint32_t type,
                                           io_connect_t *port) {
    assert(service == 42 && !type && !port_open);
    ++opens; port_open = true; *port = test_port; return KERN_SUCCESS;
}
extern "C" kern_return_t test_service_close(io_connect_t port) {
    assert(port == test_port && port_open);
    ++closes; port_open = false; return KERN_SUCCESS;
}
extern "C" kern_return_t test_vm_map(vm_map_t, mach_vm_address_t *address, mach_vm_size_t size,
    mach_vm_offset_t, int, mem_entry_name_port_t, memory_object_offset_t, boolean_t, vm_prot_t,
    vm_prot_t, vm_inherit_t) {
    assert(size == window_bytes); *address = window_base; return KERN_SUCCESS;
}
extern "C" kern_return_t test_vm_deallocate(vm_map_t, mach_vm_address_t address, mach_vm_size_t size) {
    assert(address == window_base && size == window_bytes); return KERN_SUCCESS;
}
extern "C" kern_return_t test_map_memory(io_connect_t port, uint32_t type, task_port_t,
    mach_vm_address_t *address, mach_vm_size_t *size, IOOptionBits options) {
    assert(port == test_port && port_open && type == MLG_FW_MAILBOX_MEMORY_TYPE);
    assert(options & kIOMapAnywhere);
    assert(!init_calls); /* mapped before InitDevice starts */
    ++maps;
    if (!mapping_supported) return kIOReturnUnsupported; /* a driver without a mailbox */
    *address = reinterpret_cast<uintptr_t>(mailbox_region);
    *size = mailbox_bytes;
    return KERN_SUCCESS;
}
extern "C" kern_return_t test_unmap_memory(io_connect_t port, uint32_t type, task_port_t,
                                           mach_vm_address_t address) {
    assert(port == test_port && port_open && type == MLG_FW_MAILBOX_MEMORY_TYPE);
    assert(address == reinterpret_cast<uintptr_t>(mailbox_region) && init_calls == 1);
    ++unmaps; return KERN_SUCCESS;
}
extern "C" kern_return_t test_scalar(mach_port_t port, uint32_t selector, const uint64_t *in,
    uint32_t inputs, uint64_t *out, uint32_t *count) {
    assert(port == test_port && port_open);
    switch (selector) {
    case 43: out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 219; break;
    case 1: out[3] = 0x1002; out[4] = 0x1234; out[6] = 0x01; break;
    case 54: assert(inputs == 1 && *count == 3);
        out[0] = in[0] ? in[0] : (ready ? window_base : 0); out[1] = window_bytes; out[2] = ready; break;
    case 9: {
        assert(!inputs && !*count && !init_calls++);
        /* The servicer attached before the probe started. */
        assert(fw_mailbox_servicer_present() == mapping_supported);
        std::thread probe(dext_probe_requests_firmware);
        probe.join(); /* InitDevice blocks its caller for the whole probe */
        ready = true;
        break;
    }
    case 21:
        assert(inputs == 1);
        if (in[0] == 4) out[0] = ready ? 2 : 0;
        else if (in[0] == 2) { out[0] = 256ull << 20; out[1] = 32ull << 30; }
        else if (in[0] == 9) {
            out[0] = 32ull << 30; out[1] = 31ull << 30; out[2] = 1ull << 30;
            out[3] = 0; out[4] = 256ull << 20; out[5] = 0;
        } else if (in[0] == 12) {
            return kIOReturnNotReady; /* no compute-session report: legacy path */
        } else assert(false);
        break;
    default: assert(false);
    }
    return KERN_SUCCESS;
}

namespace mac_hsa { namespace {
struct IdleDiagnosticAccess {
    static void initialize(bool supported) {
        mapping_supported = supported;
        ready = false; maps = unmaps = init_calls = 0;
        dext = {};
        assert(fw_mailbox_attach(mailbox_region, mailbox_bytes) == 0);
        {
            IOKitConnection connection(OriginalAtomicCaps{}, true);
            connection.service = 42;
            assert(connection.ensureReady() == HSA_STATUS_SUCCESS);
            assert(connection.state == IOKitConnection::State::Ready);
            assert(!connection.firmwareService);
            /* Converted-protocol uploads are refused on the shim path. */
            assert(connection.prepareFirmware({{0, "x.bin"}}) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
            assert(connection.uploadFirmware({0, "x.bin"}) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
            connection.service = 0; /* The synthetic service has no OS reference. */
        }
        assert(init_calls == 1 && maps == 1 && unmaps == (supported ? 1u : 0u));
        assert(!fw_mailbox_servicer_present()); /* detached after InitDevice */
        assert(opens == closes && !port_open);
        fw_mailbox_detach();
    }
};
} }

int main() {
    char root[] = "/tmp/hsa-fw-root.XXXXXX";
    assert(mkdtemp(root));
    write_image(root);
    assert(setenv(MLG_FW_ROOT_ENV, root, 1) == 0);
    mailbox_region = static_cast<uint8_t *>(aligned_alloc(16384, mailbox_bytes));
    assert(mailbox_region);
    fw_mailbox_set_timeouts(10000, 5000);

    mac_hsa::IdleDiagnosticAccess::initialize(true);
    assert(dext.present == 0 && dext.equal && dext.size == image.size());
    assert(dext.absent == -ENOENT); /* a missing file fails like Linux */
    assert(dext.escaped == -EINVAL); /* never resolved outside the root */
    std::puts("InitDevice firmware requests were served from the firmware root while selector 9 blocked");

    /* A driver without a mailbox: initialization proceeds, requests miss
     * immediately and the driver keeps its embedded fallback. */
    mac_hsa::IdleDiagnosticAccess::initialize(false);
    assert(dext.present == -ENOENT && dext.absent == -ENOENT);
    std::puts("A driver without a firmware mailbox still initializes (embedded fallback only)");

    std::free(mailbox_region);
    std::remove((std::string(root) + "/" + present_name).c_str());
    rmdir((std::string(root) + "/amdgpu").c_str());
    rmdir(root);
}
