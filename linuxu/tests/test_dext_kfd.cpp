/* The dext's KFD-backed queue ABI (dext/sources/dext_kfd.mm) over a real
 * KFD compute session and the unmodified upstream KFD.
 *
 * Runs what selectors 56-59 and 55 do for a KFD-backed client: the
 * runtime's ring and amd_queue_t page as GTT BOs at CPU VA == GPU VA, two
 * persistent queues (two MES ADD_QUEUEs, no legacy HQD), the dext's
 * amd_queue_t device fields, a dispatch the fixture's command processor
 * executes after the kick reached the queue's KFD doorbell, a scratch
 * request served from KFD VRAM, bounded launches on a short-lived KFD queue
 * (completed, and timed out then destroyed through KFD without leaving the
 * session uncertain), and the client close. The amdgpu device is
 * kfd_session_fixture.c; DriverKit's allocator comes from a stub header. */
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "kfd_session_fixture.h"
#include "../../dext/sources/dext_kfd.h"
#include "../../dext/sources/dext_aql.h"
#include "../../dext/amdgpu/amdgpu_aql_packets.h"
#include <rt/compute.h>
#include <rt/queue.h>

static_assert(offsetof(amd_queue_t, hsa_queue.base_address) == FIXTURE_AQL_RING_BASE);
static_assert(offsetof(amd_queue_t, hsa_queue.size) == FIXTURE_AQL_RING_SIZE);
static_assert(offsetof(amd_queue_t, write_dispatch_id) == FIXTURE_AQL_WRITE_ID);
static_assert(offsetof(amd_queue_t, read_dispatch_id) == FIXTURE_AQL_READ_ID);
static_assert(sizeof(amd_queue_t) == FIXTURE_AQL_QUEUE_BYTES);
static_assert(offsetof(hsa_kernel_dispatch_packet_t, completion_signal) == FIXTURE_AQL_COMPLETION);
static_assert(HSA_PACKET_TYPE_INVALID == FIXTURE_AQL_PACKET_INVALID &&
              HSA_PACKET_TYPE_KERNEL_DISPATCH == FIXTURE_AQL_PACKET_DISPATCH);

/* What the dext's backend gets from the probed device (rt/compute.h,
 * rt/queue.h): a GFX 12.0.1 device with 4 engines and 64 CUs. */
extern "C" const struct rt_tmpring_layout rt_tmpring_gc12;
extern "C" struct amdgpu_device *rt_compute_device(struct rt_compute_ctx *ctx)
{
    assert(ctx == fixture_compute_ctx());
    return fixture_adev();
}
extern "C" int rt_device_topology(struct amdgpu_device *adev, struct rt_compute_topology *out)
{
    assert(adev == fixture_adev());
    *out = {};
    out->gfx_target_version = 120001;
    return 0;
}
extern "C" int rt_queue_device_geometry(struct amdgpu_device *adev, struct rt_queue_geometry *out)
{
    assert(adev == fixture_adev());
    *out = {};
    out->gfx_major = 12; out->gfx_minor = 0; out->gfx_revision = 1;
    out->engines = 4; out->compute_units = 64; out->waves_per_cu = 32;
    out->tmpring = rt_tmpring_gc12;
    return 0;
}

template <typename T> static T *host(uint64_t va)
{
    void *p = fixture_va_to_host(fixture_pasid_of(va), va, sizeof(T));
    assert(p);
    return static_cast<T *>(p);
}

struct RuntimeQueue { rt_kfd_bo *ring, *metadata; uint64_t ringVA, metadataVA; dext_kfd_queue *queue; };

/* hsa_queue_create: the ring and amd_queue_t page as shared buffers. */
static void runtime_queue(dext_kfd_client *c, RuntimeQueue &q, uint32_t packets, uint32_t scratch)
{
    assert(!dext_kfd_bo_alloc(c, uint64_t(packets) * 64, 16384, 2, &q.ring, &q.ringVA));
    assert(!dext_kfd_bo_alloc(c, 16384, 16384, 2, &q.metadata, &q.metadataVA));
    auto *m = host<amd_queue_t>(q.metadataVA);
    std::memset(m, 0, sizeof(*m));
    m->hsa_queue.type = HSA_QUEUE_TYPE_MULTI;
    m->hsa_queue.features = HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
    m->hsa_queue.base_address = reinterpret_cast<void *>(q.ringVA);
    m->hsa_queue.size = packets;
    m->queue_properties = AMD_QUEUE_PROPERTIES_IS_PTR64;
    m->read_dispatch_id_field_base_byte_offset = offsetof(amd_queue_t, read_dispatch_id);
    m->scratch_wave64_lane_byte_size = scratch;
    for (uint32_t i = 0; i < packets; ++i)
        host<hsa_kernel_dispatch_packet_t>(q.ringVA + i * 64)->header = HSA_PACKET_TYPE_INVALID;
    assert(!dext_kfd_queue_create(c, q.ring, q.metadata, packets, &q.queue));
}

static void publish(const RuntimeQueue &q, uint64_t index, uint64_t code, uint64_t signal,
                    uint32_t privateBytes)
{
    auto *m = host<amd_queue_t>(q.metadataVA);
    auto *p = host<hsa_kernel_dispatch_packet_t>(q.ringVA + (index % m->hsa_queue.size) * 64);
    hsa_kernel_dispatch_packet_t packet{};
    packet.setup = 1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x = packet.workgroup_size_y = packet.workgroup_size_z = 1;
    packet.grid_size_x = packet.grid_size_y = packet.grid_size_z = 1;
    packet.private_segment_size = privateBytes;
    packet.kernel_object = code;
    packet.completion_signal.handle = signal;
    std::memcpy(reinterpret_cast<char *>(p) + 4, reinterpret_cast<char *>(&packet) + 4, 60);
    __atomic_store_n(&p->header, uint16_t(HSA_PACKET_TYPE_KERNEL_DISPATCH), __ATOMIC_RELEASE);
    __atomic_store_n(&m->write_dispatch_id, index + 1, __ATOMIC_RELEASE);
}

template <typename T> static bool wait_value(const volatile T *value, T expected)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected)
        if (std::chrono::steady_clock::now() > deadline) return false;
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
    return true;
}

int main()
{
    using namespace amdgpu;
    fixture_device_init();
    fixture_kfd_init();
    const size_t live = fixture_kmalloc_live();
    fixture_kfd_wq_init();
    fixture_cp_start();
    rt_compute_ctx *ctx = fixture_compute_ctx();

    assert(!dext_kfd_supported(ctx));
    dext_kfd_client *c = nullptr;
    assert(!dext_kfd_open(ctx, 4242, "hrx-client", &c) && c);
    struct dext_kfd_info info{};
    assert(!dext_kfd_info(c, &info) && info.pid == 4242 && info.slots == 127 && !info.window_base);
    assert(!dext_kfd_set_window(c, info.window_size * 4, 0));
    struct dext_aql_limits abi{};
    assert(!dext_kfd_queue_abi(&abi) && abi.min_packets == 64 && abi.max_packets == 4096);

    /* Two persistent queues: two MES ADD_QUEUEs. */
    RuntimeQueue q0{}, q1{};
    runtime_queue(c, q0, 64, 0);
    runtime_queue(c, q1, 256, 0);
    assert(fixture_mes_adds() == 2 && dext_kfd_queue_count(c) == 2);
    /* The dext filled the device fields: KFD's flat apertures (v9 layout on
     * GFX 12.0), the CU/wave limits, and an inactive signal in the private
     * range. */
    const auto *m0 = host<amd_queue_t>(q0.metadataVA);
    assert(m0->group_segment_aperture_base_hi == 0x10000 &&
           m0->private_segment_aperture_base_hi == 0x20000);
    assert(m0->max_cu_id == 63 && m0->max_wave_id == 31);
    assert(m0->queue_inactive_signal.handle >= (3ULL << 45) && !m0->compute_tmpring_size);
    assert(host<amd_queue_t>(q1.metadataVA)->queue_inactive_signal.handle !=
           m0->queue_inactive_signal.handle);

    /* A dispatch on q0 runs once the kick reaches its doorbell. */
    rt_kfd_bo *code = nullptr, *signals = nullptr, *kernarg = nullptr;
    uint64_t codeVA = 0, signalsVA = 0, kernargVA = 0;
    assert(!dext_kfd_bo_alloc(c, 16384, 16384, 3, &code, &codeVA));
    assert(!dext_kfd_bo_alloc(c, 16384, 16384, 2, &signals, &signalsVA));
    assert(!dext_kfd_bo_alloc(c, 16384, 16384, 3, &kernarg, &kernargVA));
    auto *signal = host<amd_signal_t>(signalsVA);
    signal->kind = AMD_SIGNAL_KIND_USER;
    signal->value = 1;
    publish(q0, 0, codeVA, signalsVA, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(signal->value == 1 && !fixture_cp_dispatches()); /* not kicked yet */
    assert(dext_kfd_queue_kick(q0.queue, 1) == -EINVAL);   /* beyond write index */
    assert(!dext_kfd_queue_kick(q0.queue, 0));
    assert(wait_value(&signal->value, int64_t(0)) && fixture_cp_dispatches() == 1);
    assert(wait_value(&m0->read_dispatch_id, uint64_t(1)));
    assert(!dext_kfd_queue_kick(q0.queue, 0)); /* already published: no rewrite */

    /* A scratch request on q1 (never kicked, so the CP leaves it alone):
     * the dext allocates KFD VRAM scratch and fills the SRD fields. */
    publish(q1, 0, codeVA, signalsVA, 60);
    auto *m1 = host<amd_queue_t>(q1.metadataVA);
    auto *inactive = host<amd_signal_t>(m1->queue_inactive_signal.handle);
    uint64_t pending = 0;
    assert(!dext_kfd_queue_service(q1.queue, &pending) && !pending); /* nothing pending */
    inactive->value = 0x401;
    assert(!dext_kfd_queue_service(q1.queue, &pending) && !pending && !inactive->value);
    assert(m1->scratch_backing_memory_location >= (3ULL << 45) &&
           m1->scratch_wave64_lane_byte_size == 64 && m1->compute_tmpring_size);
    inactive->value = 0x2;
    assert(dext_kfd_queue_service(q1.queue, &pending) == -EIO);
    inactive->value = 0;

    /* Bounded launch (selector 55) on a short-lived KFD queue. */
    AQLDispatchRequest request{};
    request.version = 1;
    request.codeHandle = request.kernargHandle = 1;
    request.kernargBytes = 16;
    request.timeoutUS = 1000000;
    for (unsigned i = 0; i < 3; ++i) request.groups[i] = request.threads[i] = 1;
    uint64_t out[5]{};
    int uncertain = 1;
    const unsigned adds = fixture_mes_adds(), removes = fixture_mes_removes();
    int launched = dext_kfd_dispatch_bounded(c, codeVA, kernargVA, &request, sizeof(request), out,
                                             &uncertain);
    if (launched)
        std::fprintf(stderr, "dext_kfd test: bounded launch %d out %llu %llu %llu %llu %llu\n", launched,
                     (unsigned long long)out[0], (unsigned long long)out[1], (unsigned long long)out[2],
                     (unsigned long long)out[3], (unsigned long long)out[4]);
    assert(!launched);
    assert(!uncertain && out[0] == 0 && out[1] == 0 && out[2] == 5 && out[3] == 0 && out[4] == 1);
    assert(fixture_mes_adds() == adds + 1 && fixture_mes_removes() == removes + 1);
    assert(fixture_cp_dispatches() == 2);
    /* Again on the same launch buffers. */
    assert(!dext_kfd_dispatch_bounded(c, codeVA, kernargVA, &request, sizeof(request), out, &uncertain));
    assert(out[2] == 5 && fixture_cp_dispatches() == 3);
    /* A launch nobody executes times out; DESTROY_QUEUE takes the queue off
     * MES, so the session stays healthy. */
    fixture_cp_stop();
    request.timeoutUS = 2000;
    assert(dext_kfd_dispatch_bounded(c, codeVA, kernargVA, &request, sizeof(request), out, &uncertain) ==
           -ETIMEDOUT);
    assert(!uncertain && !dext_kfd_uncertain(c) && out[0] == ETIMEDOUT);
    assert(fixture_mes_adds() == fixture_mes_removes() + 2);
    fixture_cp_start();
    request.timeoutUS = 1000000;
    assert(!dext_kfd_dispatch_bounded(c, codeVA, kernargVA, &request, sizeof(request), out, &uncertain));
    assert(out[2] == 5);

    /* Destroy one queue, then close the client with the other alive. */
    assert(!dext_kfd_queue_destroy(q0.queue));
    assert(dext_kfd_queue_count(c) == 1);
    assert(!dext_kfd_bo_free(c, q0.ring) && !dext_kfd_bo_free(c, q0.metadata));
    assert(!dext_kfd_close(c));
    assert(fixture_mes_adds() == fixture_mes_removes());

    fixture_cp_stop();
    fixture_kfd_release_processes();
    assert(!fixture_render_balance());
    if (fixture_kmalloc_live() != live)
        std::fprintf(stderr, "dext_kfd test: %zu kmalloc bytes live, %zu before\n",
                     fixture_kmalloc_live(), live);
    assert(fixture_kmalloc_live() == live);
    fixture_kfd_exit();
    fixture_device_fini();
    assert(!fixture_report_bos() && !fixture_kernel_allocs());
    std::puts("dext KFD queues over upstream KFD: two MES queues, amd_queue_t device fields, kick to "
              "the KFD doorbell, scratch from KFD VRAM, bounded launches and close passed");
    return 0;
}
