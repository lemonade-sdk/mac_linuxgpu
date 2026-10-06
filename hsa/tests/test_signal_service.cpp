// Host unit test: the GPU signal service's retirement while another thread
// holds the session. A transfer of a large buffer holds the transport's
// session lock for its whole length, so the queue service call the
// retirement makes waits behind it, past the retirement's 100 ms budget.
// The service kernel exited long before; the retirement must see its
// completion and stay healthy, not fail every GPU signal of the process
// (whose waits then return at once, with no work done).
//
// The "GPU" here is a thread that runs the mailbox protocol the service
// kernel runs (signal_mailbox_layout.h) on the queue the service creates.

#include "gpu_signal_service.h"
#include "signal_kernels.h"
#include "signal_mailbox_layout.h"
#include "signal_state.h"
#include <hsa/amd_hsa_queue.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", std::string(msg).c_str(), __LINE__); ++failures; } \
    else { std::fprintf(stderr, "  ok: %s\n", std::string(msg).c_str()); } \
} while (0)

namespace {
using namespace std::chrono_literals;
template <typename T> std::atomic_ref<T> word(T &value) { return std::atomic_ref<T>(value); }

class ServiceConnection final : public mac_hsa::Connection {
public:
    std::chrono::milliseconds serviceDelay{0};
    std::atomic<unsigned> serviceCalls{0};
    hsa_status_t read(mac_hsa::DeviceSnapshot &) override { return HSA_STATUS_SUCCESS; }
    hsa_status_t allocateBuffer(uint64_t size, mac_hsa::DeviceBuffer &out) override {
        void *memory = std::aligned_alloc(16384, (size + 16383) & ~uint64_t(16383));
        if (!memory) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        out = {++handles, reinterpret_cast<uintptr_t>(memory), size};
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const mac_hsa::DeviceBuffer &buffer) override {
        std::free(reinterpret_cast<void *>(buffer.address));
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const mac_hsa::DeviceBuffer &buffer, uint64_t offset, const void *src,
                             size_t size) override {
        std::memcpy(reinterpret_cast<char *>(buffer.address) + offset, src, size);
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateSharedBuffer(uint64_t size, mac_hsa::SharedBuffer &out) override {
        const auto status = allocateBuffer(size, out.device);
        out.host = reinterpret_cast<void *>(out.device.address);
        return status;
    }
    hsa_status_t freeSharedBuffer(const mac_hsa::SharedBuffer &buffer) override {
        return freeBuffer(buffer.device);
    }
    hsa_status_t createQueue(const mac_hsa::SharedBuffer &ring, const mac_hsa::SharedBuffer &,
                             uint32_t, uint64_t &handle) override {
        this->ring = static_cast<hsa_kernel_dispatch_packet_t *>(ring.host);
        handle = ++handles;
        return HSA_STATUS_SUCCESS;
    }
    // The service kernel: report ready, apply requests to the arena, exit on
    // the cancel and let the command processor write the completion.
    hsa_status_t kickQueue(uint64_t, uint64_t) override {
        const auto &packet = ring[0];
        auto *args = static_cast<uint64_t *>(packet.kernarg_address);
        auto *mailbox = reinterpret_cast<uint64_t *>(args[0]);
        auto *arena = reinterpret_cast<mac_hsa::SignalABI *>(args[1]);
        auto *done = reinterpret_cast<mac_hsa::SignalABI *>(packet.completion_signal.handle);
        gpu = std::thread([=] {
            word(mailbox[MAC_MAILBOX_READY]).store(1, std::memory_order_release);
            uint64_t completed = 0;
            for (;;) {
                if (word(mailbox[MAC_MAILBOX_ABORT]).load(std::memory_order_acquire)) break;
                const uint64_t sequence = word(mailbox[MAC_MAILBOX_REQUEST_SEQUENCE]).load(std::memory_order_acquire);
                if (sequence == completed) { std::this_thread::sleep_for(10us); continue; }
                auto &value = arena[mailbox[MAC_MAILBOX_SLOT]].value;
                for (uint64_t i = 0; i < mailbox[MAC_MAILBOX_COUNT]; ++i) {
                    const uint64_t *request = &mailbox[MAC_MAILBOX_REQUESTS + i * MAC_MAILBOX_REQUEST_STRIDE];
                    mailbox[MAC_MAILBOX_RESULTS + i] = uint64_t(value);
                    if (request[0] == 1) value = int64_t(request[1]);
                }
                completed = sequence;
                word(mailbox[MAC_MAILBOX_COMPLETION_SEQUENCE]).store(completed, std::memory_order_release);
            }
            word(done->value).store(0, std::memory_order_release);
        });
        return HSA_STATUS_SUCCESS;
    }
    // Waits as long as a transfer on another thread holds the session.
    hsa_status_t serviceQueue(uint64_t, uint64_t &inactive) override {
        ++serviceCalls;
        std::this_thread::sleep_for(serviceDelay);
        inactive = 0;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t) override {
        if (gpu.joinable()) gpu.join();
        return HSA_STATUS_SUCCESS;
    }
    ~ServiceConnection() override { if (gpu.joinable()) gpu.join(); }
private:
    uint64_t handles = 0;
    hsa_kernel_dispatch_packet_t *ring = nullptr;
    std::thread gpu;
};
}

int main() {
    mac_hsa::IsaTarget isa;
    CHECK(mac_hsa::resolveIsaTarget(120001, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa),
          "ISA for a GFX12 device");
    mac_hsa::SignalKernelObjects objects;
    CHECK(mac_hsa::selectSignalKernels(isa, objects) && !objects.mailbox.empty(), "bundled mailbox kernel");
    if (failures) return 1;

    auto connection = std::make_shared<ServiceConnection>();
    mac_hsa::SharedBuffer arena{};
    CHECK(connection->allocateSharedBuffer(16384, arena) == HSA_STATUS_SUCCESS, "signal arena");
    std::memset(arena.host, 0, 16384);
    {
        mac_hsa::GPUSignalService service(connection, arena, isa, objects.mailbox, 1h);
        int64_t old = -1;
        CHECK(service.execute(3, 1, 7, 0, old) == mac_hsa::SignalServiceResult::Success && old == 0,
              "a store through the service");
        CHECK(static_cast<mac_hsa::SignalABI *>(arena.host)[3].value == 7, "the store reached the arena");

        // Retire while every queue service call waits 150 ms on the session.
        connection->serviceDelay = 150ms;
        const auto start = std::chrono::steady_clock::now();
        const bool retired = service.reclaim();
        const auto took = std::chrono::steady_clock::now() - start;
        std::fprintf(stderr, "  retirement took %lld ms, %u queue service call(s)\n",
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(took).count(),
                     connection->serviceCalls.load());
        CHECK(retired, "retirement behind a held session confirms the kernel's completion");
        CHECK(service.healthy(), "the service stays healthy");

        connection->serviceDelay = 0ms;
        old = -1;
        CHECK(service.execute(3, 1, 9, 0, old) == mac_hsa::SignalServiceResult::Success && old == 7,
              "the service starts again for the next operation");
        CHECK(service.shutdown(), "clean shutdown");
    }
    connection->freeSharedBuffer(arena);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "all signal service checks passed\n");
    return 0;
}
