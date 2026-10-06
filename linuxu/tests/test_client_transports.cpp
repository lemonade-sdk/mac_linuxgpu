/* The driver's clients' real IOKit transports against the dext's real
 * dispatch (dext/sources/MacLinuxGPUXcode.mm: ExternalMethod's delivery
 * part, lx_external_method, the owner calls; extracted by
 * scripts/test-client-transports.sh), with only what lies behind the
 * dispatch (the GPU's session answers, the Linux process) and DriverKit
 * mocked:
 *   - the HSA runtime (hsa/src, unchanged) bringing a cold GPU up and
 *     creating queues as LemonSeed Engine's hrx does. On build 245 its
 *     second hsa_queue_create failed with HSA_STATUS_ERROR_OUT_OF_RESOURCES:
 *     the compute topology's 16 words did not fit an owner call's
 *     completion, so the runtime never learned the driver's queue slots and
 *     allowed one queue;
 *   - libmlg_drm (libmlg_drm/src/mlg_transport_iokit.c, mlg_drm.c,
 *     mlg_init.c, unchanged).
 * Both reach the driver through host/selector_call.h and owner_call.h.
 *
 * Between them sits a test kernel standing for IOKit: every
 * IOConnectCall*Method the library makes reaches the dext's ExternalMethod
 * on one delivery thread, an async one with an OSAction whose completion
 * comes back to the library's notification port as a Mach message. The
 * kernel fails the test the moment an async call returns from the dext
 * answered and with nothing left to complete it: build 243's libmlg_drm
 * sent LX_MMAP_COMMIT async, the dext answered it on the call, and every
 * Vulkan client waited for good at its first BO map. Loopback transports
 * (test-mlg-drm) cannot see that; this one runs both real sides.
 *
 * Covered: the first open of an uninitialized GPU (HostWindow, InitDevice,
 * then the open again), sync and async ioctls (inline and LX_RESULT
 * replies), mmap with LX_MMAP_COMMIT, munmap, close, LX_SCANOUT, from
 * several threads at once. */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/mman.h>

#include "session_state.h"
#include "observer_gate.h"
#include "dext_compute.h"
#include <rt/lx_abi.h>
#include <mlg_drm.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <mac_hsa.h>
#include "signal_kernels.h"
#include "selector_call.h"

#include <Block.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

/* The Linux process behind a Linux-file client (rt/lx_files.h), mocked. */
extern "C" {
struct rt_lx_client;
struct pci_dev;
struct rt_lx_display_hooks { int unused; };
typedef void (*rt_lx_done_fn)(void *ctx, uint64_t token, int64_t result, const void *rbuf,
                              size_t reply_bytes);
int mlg_lx_frame_check(const void *frame, size_t bytes, uint32_t cmd, uint64_t *out_bytes);
int mlg_lx_cmd_sleeps(uint32_t dev, uint32_t cmd, const void *frame, size_t bytes);
}
enum { RT_LX_HOP_ADMIT, RT_LX_HOP_ARGS, RT_LX_HOP_REPLY, RT_LX_HOP_TOTAL };

#define MACLINUXGPU_LOG(...) (std::printf("dext: " __VA_ARGS__), std::printf("\n"))
#define MACLINUXGPU_EVENT(...) (std::printf("dext event: " __VA_ARGS__), std::printf("\n"))

static void fail(const char *what)
{
    std::fprintf(stderr, "FAIL client transports: %s\n", what);
    std::fflush(stderr);
    std::_Exit(1);
}
#define CHECK(c) ((c) ? (void)0 : fail(#c))

// ---- DriverKit mocks ----
struct OSObjectMock {
    std::atomic<int> refs{1};
    virtual ~OSObjectMock() = default;
    void retain() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
struct OSObject : OSObjectMock {};
struct OSData : OSObjectMock {
    std::vector<uint8_t> bytes;
    static OSData *withBytes(const void *p, size_t n) {
        auto *d = new OSData;
        d->bytes.assign(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
        return d;
    }
    size_t getLength() const { return bytes.size(); }
    const void *getBytesNoCopy() const { return bytes.data(); }
};
enum { kIOMemoryMapReadOnly = 1 };
struct IOMemoryMap : OSObjectMock {
    uint64_t address = 0, length = 0;
    uint64_t GetAddress() { return address; }
    uint64_t GetLength() { return length; }
};
// A client buffer above 4096 bytes, as the kernel hands it to the dext.
struct IOMemoryDescriptor : OSObjectMock {
    void *buffer = nullptr;
    uint64_t length = 0;
    kern_return_t GetLength(uint64_t *out) { *out = length; return kIOReturnSuccess; }
    kern_return_t CreateMapping(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, IOMemoryMap **map) {
        auto *m = new IOMemoryMap;
        m->address = (uint64_t)(uintptr_t)buffer;
        m->length = length;
        *map = m;
        return kIOReturnSuccess;
    }
};
// The completion of an async call: where the kernel sends it.
struct OSAction : OSObjectMock {
    mach_port_t port = MACH_PORT_NULL;
    uint64_t callout = 0, refcon = 0;
    uint32_t selector = 0;
    std::atomic<bool> completed{false};
};
using IOUserClientAsyncArgumentsArray = uint64_t[16];
static constexpr uint64_t kIOUserClientMethodArgumentsCurrentVersion = 2;
struct IOUserClientMethodArguments {
    uint64_t version;
    uint64_t selector;
    OSAction *completion;
    const uint64_t *scalarInput;
    uint32_t scalarInputCount;
    OSData *structureInput;
    IOMemoryDescriptor *structureInputDescriptor;
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
    OSData *structureOutput;
    IOMemoryDescriptor *structureOutputDescriptor;
    uint64_t structureOutputMaximumSize;
};
struct IOUserClientMethodDispatch;
struct IOLock { std::mutex m; };
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *l) { l->m.lock(); }
static void IOLockUnlock(IOLock *l) { l->m.unlock(); }
static void *IOMalloc(size_t n) { return malloc(n); }
static void *IOMallocZero(size_t n) { return calloc(1, n); }
static void IOFree(void *p, size_t) { free(p); }

// The client's queue for the owner's calls: a thread that runs them in order.
static std::thread::id ownerThread;
struct IODispatchQueue {
    using Block = void (^)(void);
    std::mutex lock;
    std::condition_variable kick;
    std::deque<Block> pending;
    bool stop = false;
    std::thread thread;
    void start() {
        thread = std::thread([this] {
            ownerThread = std::this_thread::get_id();
            std::unique_lock guard(lock);
            for (;;) {
                kick.wait(guard, [this] { return stop || !pending.empty(); });
                if (pending.empty()) return;
                Block block = pending.front();
                pending.pop_front();
                guard.unlock();
                block();
                Block_release(block);
                guard.lock();
            }
        });
    }
    void DispatchAsync(Block block) {
        std::lock_guard guard(lock);
        pending.push_back(Block_copy(block));
        kick.notify_one();
    }
    void finish() {
        { std::lock_guard guard(lock); stop = true; kick.notify_one(); }
        thread.join();
    }
};

struct OwnerResult;
struct ClientMemory;
struct MacLinuxGPUUserClient;
struct MacLinuxGPU;
// A Retire's delivery starts the Stop/Retire watchdog (session shutdown
// checks it): nothing to watch here.
static void session_watchdog_start(MacLinuxGPU *, const char *, bool) {}
struct MacLinuxGPUUserClient_IVars {
    MacLinuxGPU *ownerDriver;
    uint64_t sessionGeneration;
    uint64_t clientID;
    bool stopping;
    bool observer;
    IODispatchQueue *ownerQueue;
    IOLock *ownerLock;
    OwnerResult *ownerResults;
    uint64_t ownerToken;
    ClientMemory *memories;
    bool syncRefusalLogged;
    bool linuxFile;
    struct rt_lx_client *lx;
    MacLinuxGPUUserClient *nextLinuxFile;
};
static void kernel_complete(OSAction *action, kern_return_t status, const uint64_t *args, uint32_t count);
struct MacLinuxGPUUserClient : OSObjectMock {
    MacLinuxGPUUserClient_IVars *ivars;
    void AsyncCompletion(OSAction *action, kern_return_t status, const IOUserClientAsyncArgumentsArray data,
                         uint32_t count) {
        kernel_complete(action, status, data, count);
    }
    kern_return_t ExternalMethod(uint64_t selector, IOUserClientMethodArguments *arguments,
                                 const IOUserClientMethodDispatch *dispatch, OSObject *target,
                                 void *reference);
};

// ---- the driver state the extracted code reads ----
static uint64_t s_sessionGeneration = 1;
// The session Disconnect GPU closed (none here: its NoDevice answer is a
// session-shutdown check).
static uint64_t s_disconnectedGeneration = 0;
static bool s_stopping, s_sessionClosing, s_dmaQuarantined;
static bool s_modulesRunning;           // set by InitDevice
static void *s_rtDevice;                // set by InitDevice
static bool s_probeAttempted;
static int s_probeResult;
static mlg_observer_gate s_lxCalls;     // opened by InitDevice
static const rt_lx_display_hooks rt_display_lx_hooks{};
static std::atomic<int> sessionOpens, initDevices, hostWindows, ownerCallsOnDelivery;
static std::thread::id deliveryThread;
static kern_return_t ensure_open(MacLinuxGPUUserClient *) { ++sessionOpens; return kIOReturnSuccess; }
static void *rt_device_get_pdev(void *device) { return device; }
static void client_creator(MacLinuxGPUUserClient *, int *pid, char *name, size_t size)
{
    *pid = 4242;
    std::snprintf(name, size, "llama-bench");
}
static int dext_pci_transport_fault() { return 0; }
static uint64_t dext_pci_transport_fault_offset() { return 0; }
static kern_return_t power_wait(MacLinuxGPUUserClient *, OSAction *, uint64_t) { return kIOReturnUnsupported; }
static void power_snapshot(uint64_t *out) { memset(out, 0, MLG_POWER_STATE_WORDS * 8); out[0] = MLG_POWER_STATE_VERSION; }
static void reset_state(uint64_t *out) { memset(out, 0, MLG_RESET_STATE_WORDS * 8); out[0] = MLG_RESET_STATE_VERSION; }
// The driver's lifecycle as NewUserClient and the session state see it.
static std::atomic<uint64_t> sessionFlagsNow{0};
static void session_state(uint64_t *out)
{
    memset(out, 0, MLG_SESSION_STATE_WORDS * 8);
    out[0] = MLG_SESSION_STATE_VERSION;
    out[1] = sessionFlagsNow.load();
}
static size_t klog_read(uint64_t *, char *, size_t, uint64_t *end) { *end = 0; return 0; }
int dext_compute_runtime_build_cached(uint64_t *out)
{
    // As the installed driver answers (DEXT_RUNTIME_BUILD): "AMDGPUAB", layout 1.
    out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 243; out[3] = 243;
    return 0;
}
static kern_return_t observer_sysfs_read(IOUserClientMethodArguments *) { return kIOReturnUnsupported; }
static kern_return_t observer_sysfs_write(IOUserClientMethodArguments *) { return kIOReturnUnsupported; }
static kern_return_t observer_drm_info(IOUserClientMethodArguments *) { return kIOReturnUnsupported; }
static kern_return_t display_call(MacLinuxGPUUserClient *, uint64_t, IOUserClientMethodArguments *) { return kIOReturnUnsupported; }
int dext_compute_bo_memory(uint32_t, void **, uint64_t *) { return -ENOENT_L; }
static IOMemoryDescriptor *copy_bo_ranges_descriptor(uint32_t, uint64_t) { return nullptr; }
static void *dext_dma_copy_descriptor(void *) { return nullptr; }
extern "C" int rt_bounded_run(void (*fn)(void *), void *arg, void (*)(void *), unsigned int)
{
    fn(arg);
    return 0;
}

// ---- the Linux process (rt/lx_files.h), mocked ----
struct rt_lx_client {
    std::mutex lock;
    int nextFd = 3;
    uint64_t nextToken = 0, nextType = MLG_LX_MMAP_TYPE_BASE;
    std::map<uint64_t, uint64_t> maps;                  // type -> length
    std::map<uint64_t, std::vector<uint8_t>> kept;      // token -> long reply
    std::map<uint64_t, int64_t> keptResult;
};
static std::mutex workersLock;
static std::vector<std::thread> &workers = *new std::vector<std::thread>;
static std::atomic<int> commits, opens, closes, mmaps, munmaps, syncIoctls, asyncIoctls, results, scanouts;
static std::map<uint64_t, uint64_t> mapLengths;        // what IOConnectMapMemory64 maps
static std::mutex mapLengthsLock;

static int rt_lx_client_create(struct pci_dev *pdev, int pid, const char *, rt_lx_client **out)
{
    CHECK(pdev && pid == 4242);
    *out = new rt_lx_client;
    return 0;
}
static void rt_lx_client_set_display(rt_lx_client *, const rt_lx_display_hooks *) {}
static int rt_lx_client_pid(const rt_lx_client *) { return 4242; }
static int rt_lx_client_retire(rt_lx_client *c, void (*)(void *), void *) { delete c; return 0; }
static uint64_t rt_lx_time_ns() { return 0; }
static void rt_lx_timing_add(uint32_t, int, uint64_t) {}

// A reply to a request frame: its OUT segments filled with a pattern the
// client checks (each byte its segment's index plus 0xa0).
static std::vector<uint8_t> reply_for(const void *frame, size_t bytes, int64_t result)
{
    mlg_lx_frame head;
    uint64_t out = 0;
    CHECK(bytes >= sizeof(head));
    memcpy(&head, frame, sizeof(head));
    CHECK(mlg_lx_frame_check(frame, bytes, head.cmd, &out) == 0);
    std::vector<uint8_t> reply(mlg_lx_reply_bytes(out));
    mlg_lx_reply rh{};
    rh.magic = MLG_LX_REPLY_MAGIC;
    rh.version = MLG_LX_VERSION;
    rh.header_bytes = sizeof(rh);
    rh.total_bytes = (uint32_t)reply.size();
    rh.result = result;
    size_t at = sizeof(rh);
    for (uint32_t i = 0; i < head.nsegs; ++i) {
        mlg_lx_segment seg;
        memcpy(&seg, (const uint8_t *)frame + sizeof(head) + i * sizeof(seg), sizeof(seg));
        if (!(seg.dir & MLG_LX_SEG_OUT)) continue;
        memset(reply.data() + at, 0xa0 + (int)i, seg.size);
        at += seg.size;
        ++rh.out_segments;
    }
    CHECK(at == reply.size());
    memcpy(reply.data(), &rh, sizeof(rh));
    return reply;
}

static void worker(std::function<void()> run)
{
    std::lock_guard guard(workersLock);
    workers.emplace_back([run] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // runs after the call returned
        run();
    });
}

static int rt_lx_op_async(rt_lx_client *c, const uint64_t *in, uint32_t nin, rt_lx_done_fn done,
                          void *ctx, uint64_t *token)
{
    if (!nin) return -MLG_LX_EINVAL;
    std::lock_guard guard(c->lock);
    const uint64_t t = *token = ++c->nextToken;
    const std::vector<uint64_t> args(in, in + nin);
    worker([c, args, done, ctx, t] {
        switch (args[0]) {
        case MLG_LX_OP_OPEN: {
            ++opens;
            CHECK(args.size() == MLG_LX_OP_OPEN_ARGS && args[1] == MLG_LX_DEV_RENDER);
            std::lock_guard g(c->lock);
            done(ctx, t, c->nextFd++, nullptr, 0);
            return;
        }
        case MLG_LX_OP_CLOSE:
            ++closes;
            done(ctx, t, 0, nullptr, 0);
            return;
        case MLG_LX_OP_MMAP: {
            ++mmaps;
            CHECK(args.size() == MLG_LX_OP_MMAP_ARGS && args[3]);
            uint64_t words[MLG_LX_OP_MMAP_WORDS];
            {
                std::lock_guard g(c->lock);
                words[0] = c->nextType++;
                words[1] = args[3];
                words[2] = 0;
                c->maps[words[0]] = args[3];
            }
            { std::lock_guard g(mapLengthsLock); mapLengths[words[0]] = args[3]; }
            done(ctx, t, 0, words, sizeof(words));
            return;
        }
        case MLG_LX_OP_MUNMAP: {
            ++munmaps;
            std::lock_guard g(c->lock);
            done(ctx, t, c->maps.erase(args[1]) ? 0 : -MLG_LX_EINVAL, nullptr, 0);
            return;
        }
        default:
            done(ctx, t, -MLG_LX_EINVAL, nullptr, 0);
        }
    });
    return 0;
}

static int rt_lx_ioctl_nosleep(rt_lx_client *, int fd, uint32_t cmd, const void *frame, size_t bytes,
                               void *rbuf, size_t cap, size_t *reply_bytes, int64_t *result)
{
    if (mlg_lx_cmd_sleeps(MLG_LX_DEV_RENDER, cmd, frame, bytes)) return -MLG_LX_EDEADLK;
    CHECK(fd >= 3);
    ++syncIoctls;
    const auto reply = reply_for(frame, bytes, 0);
    if (reply.size() > cap) return -MLG_LX_E2BIG;
    memcpy(rbuf, reply.data(), reply.size());
    *reply_bytes = reply.size();
    *result = 0;
    return 0;
}

static int rt_lx_ioctl_async(rt_lx_client *c, int fd, uint32_t, const void *frame, size_t bytes,
                             rt_lx_done_fn done, void *ctx, uint64_t *token)
{
    CHECK(fd >= 3);
    std::lock_guard guard(c->lock);
    const uint64_t t = *token = ++c->nextToken;
    const std::vector<uint8_t> request((const uint8_t *)frame, (const uint8_t *)frame + bytes);
    worker([c, request, done, ctx, t] {
        ++asyncIoctls;
        const auto reply = reply_for(request.data(), request.size(), 7);
        if (reply.size() <= MLG_LX_ASYNC_INLINE_BYTES) {
            done(ctx, t, 7, reply.data(), reply.size());
            return;
        }
        { std::lock_guard g(c->lock); c->kept[t] = reply; c->keptResult[t] = 7; }
        done(ctx, t, 7, nullptr, reply.size());
    });
    return 0;
}

static int rt_lx_result(rt_lx_client *c, uint64_t token, void *rbuf, size_t cap, size_t *bytes,
                        int64_t *result)
{
    std::lock_guard guard(c->lock);
    auto it = c->kept.find(token);
    if (it == c->kept.end()) return -MLG_LX_ENOENT;
    if (it->second.size() > cap) return -MLG_LX_ENOSPC;
    ++results;
    memcpy(rbuf, it->second.data(), it->second.size());
    *bytes = it->second.size();
    *result = c->keptResult[token];
    c->kept.erase(it);
    return 0;
}

static int rt_lx_mmap_commit(rt_lx_client *c, uint64_t type, uint64_t va)
{
    std::lock_guard guard(c->lock);
    CHECK(c->maps.count(type) && va);
    ++commits;
    return 0;
}

static int rt_lx_scanout(rt_lx_client *, const mlg_lx_scanout *, mlg_lx_scanout_state *state)
{
    ++scanouts;
    memset(state, 0, sizeof(*state));
    return -MLG_LX_ENODEV;  // no display in this test
}

// ---- the dext's code, as it ships ----
// The running session call, for the Stop/Retire watchdog (session
// shutdown checks it).
static uint64_t s_ownerJobSince;
static uint32_t s_ownerJobSelector;
static uint64_t power_now_ns() { return 1; }
#include "lx_transport_production.inc"

// ---- the GPU behind the owner's queue (ExternalMethod's session part) ----
// Answers as the installed 245 driver gave them to the HSA runtime on an
// R9700 (QueryInfo tags, GetIdentity, BO and queue calls), each refusing a
// call that asks for fewer scalars than it answers, as the driver's
// handlers do (dext_compute_query_info's out_cap checks).
static std::atomic<uint64_t> gpuStage{0};      // QueryInfo tag 4: 0 cold, 2 up
static const uint64_t kWindowBytes = 1ull << 30;
static uint64_t gartWindow, kfdWindow;         // set by HostWindow
static std::mutex sessionLock;
struct MockBo { uint64_t size, domain, gpu; };
static std::map<uint64_t, MockBo> bos;
static uint64_t nextBo = 0x47, nextVram = 0x600000000000ull, gttUsed, nextQueue = 6, nextEvent = 1;
static std::map<uint64_t, std::pair<uint64_t, uint64_t>> boMaps;  // type -> (address, size)
static uint64_t nextBoType = 0x1002f;
static std::atomic<int> queuesCreated, queuesDestroyed, topologyAnswers, eventWaits, combinedAnswers;
static const uint64_t kQueueSlots = 24;
static const uint64_t kTestTag = 0x7e57;       // a test-only tag: 14 scalars and a structure

static void go_cold()
{
    gpuStage = 0;
    s_modulesRunning = false;
    s_rtDevice = nullptr;
    s_lxCalls.close();
    gartWindow = kfdWindow = 0;
}
static void go_up()
{
    ++initDevices;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the probe
    s_modulesRunning = true;
    s_probeAttempted = true;
    s_rtDevice = (void *)0x1000;
    s_lxCalls.open();
    gpuStage = 2;
}

static kern_return_t answer(IOUserClientMethodArguments *a, std::initializer_list<uint64_t> words)
{
    if (a->scalarOutputCount < words.size()) return kIOReturnBadArgument;  // -EINVAL from the handler
    uint32_t i = 0;
    for (uint64_t w : words) a->scalarOutput[i++] = w;
    a->scalarOutputCount = i;
    return kIOReturnSuccess;
}

static kern_return_t query_info(IOUserClientMethodArguments *a)
{
    if (a->scalarInputCount < 1) return kIOReturnBadArgument;
    switch (a->scalarInput[0]) {
    case 1: return answer(a, {0xc, 0, 1});
    case 2: return answer(a, {0x10000000, 0x7f7000000});
    case 4: return answer(a, {gpuStage.load()});
    case 6: return answer(a, {0x7551, 0xc0, 0x500, 0, 0x40, 4, 2, 0x5f5e100, 0x20, 0x20});
    case 9: return answer(a, {0x7f7000000, 0x7f2954000, 0x692c000, 0x7efed4000, 0x10000000, 0x32dc000});
    case 10: {  /* the compute topology: 16 words (dext_compute_backend.inc) */
        const kern_return_t kr = answer(a, {1, 120001, 2, 16, 65536, 262128, 32, kQueueSlots,
                                            64 | (4096ull << 32), 32768, 8388608, 67108864, 2920, 2, 1, 0});
        if (kr == kIOReturnSuccess) ++topologyAnswers;
        return kr;
    }
    case 11: {
        char name[64] = "AMD Radeon AI PRO R9700";
        if (a->scalarOutputCount < 8) return kIOReturnBadArgument;
        memcpy(a->scalarOutput, name, 64);
        a->scalarOutputCount = 8;
        return kIOReturnSuccess;
    }
    case 12:
        return answer(a, {1, 2, 0x7f, 4242, kfdWindow, kWindowBytes, 0x10000, 0x7fffffffffffull});
    case MLG_QUERY_DEVICE_SPEC: {  /* a structure, as the driver's QueryInfo handler answers it */
        mlg_device_spec spec{};
        spec.version = MLG_DEVICE_SPEC_VERSION;
        spec.present = MLG_DEVICE_SPEC_GEOMETRY | MLG_DEVICE_SPEC_CUS | MLG_DEVICE_SPEC_SHADER_ARRAYS |
                       MLG_DEVICE_SPEC_SA_DISABLE | MLG_DEVICE_SPEC_BACKENDS;
        spec.shader_engines = 4; spec.shader_arrays_per_se = 2; spec.backends_per_se = 4;
        spec.cus_per_array = 8; spec.wavefront_size = 32; spec.max_waves_per_simd = 16;
        spec.scratch_slots_per_cu = 32; spec.lds_bytes = 65536; spec.active_cus = 64;
        for (unsigned se = 0; se < 4; ++se)
            for (unsigned sa = 0; sa < 2; ++sa) spec.cu_bitmap[se][sa] = 0xff00 | (se << 4) | sa;
        spec.active_sa_bitmap = 0xff; spec.cc_sa_disable = 0x12340000; spec.user_sa_disable = 0x56780000;
        spec.active_rb_bitmap = 0xffff; spec.active_rbs = 16;
        const size_t n = a->structureOutputMaximumSize < sizeof(spec) ? (size_t)a->structureOutputMaximumSize
                                                                      : sizeof(spec);
        if (n < 16) return kIOReturnBadArgument;
        spec.size = (uint32_t)n;
        a->structureOutput = OSData::withBytes(&spec, n);
        if (a->scalarOutputCount >= 1) { a->scalarOutput[0] = n; a->scalarOutputCount = 1; }
        else a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    case kTestTag: {
        uint8_t bytes[100];
        for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(0x30 + i);
        if (a->structureOutputMaximumSize < sizeof(bytes)) return kIOReturnNoSpace;
        a->structureOutput = OSData::withBytes(bytes, sizeof(bytes));
        ++combinedAnswers;
        return answer(a, {0x900, 0x901, 0x902, 0x903, 0x904, 0x905, 0x906, 0x907, 0x908, 0x909,
                          0x90a, 0x90b, 0x90c, 0x90d});
    }
    default:
        return kIOReturnUnsupported;
    }
}

static kern_return_t host_window(IOUserClientMethodArguments *a)
{
    ++hostWindows;
    CHECK(a->scalarInputCount == 1 && a->scalarOutputCount >= 3);
    const bool up = gpuStage == 2;
    uint64_t &base = up ? kfdWindow : gartWindow;
    if (a->scalarInput[0]) base = a->scalarInput[0];
    return answer(a, {base, kWindowBytes, up ? 1u : 0u});
}

// EVENT_WAIT's registration on the owner's queue; the wait ends on a
// thread of its own (event_wait_main), here at once, timed out.
static kern_return_t event_wait(IOUserClientMethodArguments *a, MacLinuxGPUUserClient *client)
{
    ++eventWaits;
    OSAction *action = a->completion;
    const uint64_t token = a->scalarInput[0];
    action->retain();
    client->retain();
    worker([action, client, token] {
        IOUserClientAsyncArgumentsArray data = {};
        data[0] = token;
        data[1] = 0;
        data[2] = 1;  // timed out
        client->AsyncCompletion(action, kIOReturnSuccess, data, MLG_EVENT_WAIT_WORDS);
        action->release();
        client->release();
    });
    a->scalarOutput[0] = 0;
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

static kern_return_t owner_session_call(MacLinuxGPUUserClient *client, uint64_t selector,
                                        IOUserClientMethodArguments *a)
{
    if (std::this_thread::get_id() == deliveryThread) ++ownerCallsOnDelivery;
    const uint64_t *in = a->scalarInput;
    std::lock_guard guard(sessionLock);
    switch (selector) {
    case kMacAMDGPUMethodHostWindow:
        return host_window(a);
    case kMacAMDGPUMethodInitDevice:
        go_up();
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    if (client->ivars->linuxFile) return kIOReturnUnsupported;
    switch (selector) {
    case kMacAMDGPUMethodGetIdentity:
        return answer(a, {gpuStage == 2 ? 5u : 0u, 0, 0, 0x1002, 0x7551, 0x30000, 0xc0});
    case kMacAMDGPUMethodQueryInfo:
        return query_info(a);
    case kMacAMDGPUMethodBOAlloc: {
        CHECK(a->scalarInputCount == 4 && in[0]);
        const uint64_t size = (in[0] + 16383) & ~16383ull;
        MockBo bo{size, in[1], 0};
        if (in[1] == 2) {  // GTT: inside the host window
            CHECK(kfdWindow && gttUsed + size <= kWindowBytes);
            bo.gpu = kfdWindow + gttUsed;
            gttUsed += size;
        } else {
            bo.gpu = nextVram;
            nextVram += size;
        }
        const uint64_t handle = nextBo++;
        bos[handle] = bo;
        return answer(a, {handle, bo.gpu, 0});
    }
    case kMacAMDGPUMethodBOFree:
        CHECK(bos.erase(in[0]) == 1);
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    case kMacAMDGPUMethodBOMap: {
        auto it = bos.find(in[0]);
        CHECK(it != bos.end());
        const uint64_t type = nextBoType++;
        boMaps[type] = {it->second.gpu, it->second.size};
        return answer(a, {type, it->second.size});
    }
    case kMacAMDGPUMethodBOCopy:
        return answer(a, {0});
    case kMacAMDGPUMethodBOWrite:
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    case kMacAMDGPUMethodBORead: {
        std::vector<uint8_t> bytes(in[2], 0x42);
        if (a->structureOutputMaximumSize < bytes.size()) return kIOReturnNoSpace;
        a->structureOutput = OSData::withBytes(bytes.data(), bytes.size());
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    case kMacAMDGPUMethodComputeDispatch:
        return answer(a, {0, 3, 3});
    case kMacAMDGPUMethodAQLQueueCreate:
        CHECK(a->scalarInputCount == 3 && bos.count(in[0]) && bos.count(in[1]));
        ++queuesCreated;
        return answer(a, {0, nextQueue++});
    case kMacAMDGPUMethodAQLQueueKick:
        return answer(a, {0});
    case kMacAMDGPUMethodAQLQueueDestroy:
        ++queuesDestroyed;
        return answer(a, {0});
    case kMacAMDGPUMethodAQLQueueService:
        return answer(a, {0, 0});
    case MLG_SELECTOR_EVENT:
        if (in[0] == 0) {
            const uint64_t id = nextEvent++;
            return answer(a, {0, id, id, 0x600000020008ull + 8 * id});
        }
        return answer(a, {0, 0, 0, 0});
    case MLG_SELECTOR_EVENT_WAIT:
        return event_wait(a, client);
    default:
        std::fprintf(stderr, "test GPU: selector %llu not modelled\n", (unsigned long long)selector);
        return kIOReturnUnsupported;
    }
}

kern_return_t MacLinuxGPUUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments *arguments,
                                                const IOUserClientMethodDispatch *dispatch,
                                                OSObject *target, void *reference)
{
    (void)target;
    (void)dispatch;
    if (!arguments) return kIOReturnBadArgument;
#include "lx_transport_delivery.inc"
    return owner_session_call(this, selector, arguments);
}

// ---- the test kernel: IOKit for the library ----
// The kernel's threads live with the process (an exiting client does not
// end the driver's): never destroyed, so exit() finds nothing to join.
static IODispatchQueue &ownerQueue = *new IODispatchQueue;  // every client's session calls (s_bringupQueue)
static std::mutex delivery;         // DriverKit delivers a driver's calls on one thread
static const io_connect_t kConnection = 0x1234;  // the Linux-file client's
static std::mutex clientsLock;
static std::map<io_connect_t, MacLinuxGPUUserClient *> clients;
static io_connect_t nextConnection = 0x2000;
static std::atomic<int> asyncCalls, syncCalls;

static MacLinuxGPUUserClient *client_of(io_connect_t connection)
{
    std::lock_guard g(clientsLock);
    auto it = clients.find(connection);
    if (it == clients.end()) fail("a call on a connection the test kernel never opened");
    return it->second;
}

struct IONotificationPort { mach_port_t port; };
struct CompletionMessage {
    mach_msg_header_t header;
    uint64_t callout, refcon;
    int32_t status;
    uint32_t count;
    uint64_t args[16];
};

static void kernel_complete(OSAction *action, kern_return_t status, const uint64_t *args, uint32_t count)
{
    CHECK(count <= 16 && !action->completed.exchange(true));
    CompletionMessage m{};
    m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.header.msgh_size = sizeof(m);
    m.header.msgh_remote_port = action->port;
    m.callout = action->callout;
    m.refcon = action->refcon;
    m.status = status;
    m.count = count;
    memcpy(m.args, args, count * sizeof(uint64_t));
    CHECK(mach_msg(&m.header, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                   MACH_PORT_NULL) == MACH_MSG_SUCCESS);
}

// One call into the dext, on the delivery thread; nothing there may wait.
static kern_return_t deliver(MacLinuxGPUUserClient *client, uint32_t selector, OSAction *action,
                             const uint64_t *input, uint32_t inputCnt,
                             const void *inputStruct, size_t inputStructCnt, uint64_t *output,
                             uint32_t *outputCnt, void *outputStruct, size_t *outputStructCnt)
{
    uint64_t out[16] = {};
    IOUserClientMethodArguments a{};
    IOMemoryDescriptor *inDesc = nullptr, *outDesc = nullptr;
    a.version = kIOUserClientMethodArgumentsCurrentVersion;
    a.selector = selector;
    a.completion = action;
    a.scalarInput = input;
    a.scalarInputCount = inputCnt;
    if (inputStructCnt > 4096) {
        inDesc = new IOMemoryDescriptor;
        inDesc->buffer = const_cast<void *>(inputStruct);
        inDesc->length = inputStructCnt;
        a.structureInputDescriptor = inDesc;
    } else if (inputStructCnt) {
        a.structureInput = OSData::withBytes(inputStruct, inputStructCnt);
    }
    a.scalarOutput = out;
    a.scalarOutputCount = outputCnt ? (*outputCnt < 16 ? *outputCnt : 16) : 0;  // IOKit's limit
    const size_t room = outputStructCnt ? *outputStructCnt : 0;
    if (room > 4096) {
        outDesc = new IOMemoryDescriptor;
        outDesc->buffer = outputStruct;
        outDesc->length = room;
        a.structureOutputDescriptor = outDesc;
    }
    a.structureOutputMaximumSize = room;
    kern_return_t kr;
    {
        std::lock_guard guard(delivery);
        deliveryThread = std::this_thread::get_id();
        const auto start = std::chrono::steady_clock::now();
        kr = client->ExternalMethod(selector, &a, nullptr, nullptr, nullptr);
        if (getenv("CLIENT_TRANSPORTS_TRACE"))
            std::fprintf(stderr, "call %u in[0]=%#llx n_in=%u -> %#x\n", selector,
                         inputCnt ? (unsigned long long)input[0] : 0ull, inputCnt, kr);
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500))
            fail("a call held the delivery thread");
        deliveryThread = std::thread::id();
    }
    if (outputCnt) {
        const uint32_t n = a.scalarOutputCount < *outputCnt ? a.scalarOutputCount : *outputCnt;
        memcpy(output, out, n * sizeof(uint64_t));
        *outputCnt = n;
    }
    if (outputStructCnt) {
        size_t length = 0;
        if (a.structureOutput) {
            length = a.structureOutput->getLength();
            if (length > room) kr = kIOReturnNoSpace;
            else memcpy(outputStruct, a.structureOutput->getBytesNoCopy(), length);
            a.structureOutput->release();
        } else if (outDesc) {
            length = room;
        }
        *outputStructCnt = length;
    } else if (a.structureOutput) {
        a.structureOutput->release();
    }
    if (a.structureInput) a.structureInput->release();
    if (inDesc) inDesc->release();
    if (outDesc) outDesc->release();
    return kr;
}

extern "C" {
// The registry: the driver's service (2) on its PCI device (3).
static std::mutex iteratorsLock;
static std::map<io_iterator_t, int> iterators;
static io_iterator_t nextIterator = 100;
kern_return_t lxt_IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef matching, io_iterator_t *it)
{
    if (matching) CFRelease(matching);
    std::lock_guard g(iteratorsLock);
    *it = nextIterator++;
    iterators[*it] = 1;
    return KERN_SUCCESS;
}
io_object_t lxt_IOIteratorNext(io_iterator_t it)
{
    std::lock_guard g(iteratorsLock);
    return iterators[it]-- > 0 ? 2 : 0;
}
kern_return_t lxt_IOObjectRelease(io_object_t) { return KERN_SUCCESS; }
kern_return_t lxt_IOObjectRetain(io_object_t) { return KERN_SUCCESS; }
boolean_t lxt_IOObjectConformsTo(io_object_t object, const io_name_t className)
{
    return object == 3 && !strcmp(className, "IOPCIDevice");
}
kern_return_t lxt_IORegistryEntryGetParentEntry(io_registry_entry_t entry, const io_name_t, io_registry_entry_t *parent)
{
    if (entry != 2) return kIOReturnNoDevice;
    *parent = 3;
    return KERN_SUCCESS;
}
kern_return_t lxt_IORegistryEntryGetName(io_registry_entry_t entry, io_name_t name)
{
    strlcpy(name, entry == 2 ? "MacLinuxGPU" : "pci1002,7551", sizeof(io_name_t));
    return KERN_SUCCESS;
}
static CFTypeRef pci_number(uint32_t value)
{
    return CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(&value), 4);
}
CFTypeRef lxt_IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef,
                                              IOOptionBits)
{
    const auto is = [key](CFStringRef name) { return CFStringCompare(key, name, 0) == kCFCompareEqualTo; };
    if (entry == 2 && is(CFSTR("IOUserClass"))) return CFRetain(CFSTR("MacLinuxGPU"));
    if (entry == 2 && is(CFSTR("CFBundleIdentifier"))) return CFRetain(CFSTR("com.lemonade.MacLinuxGPU.driver"));
    if (entry == 3 && is(CFSTR("vendor-id"))) return pci_number(0x1002);
    if (entry == 3 && is(CFSTR("device-id"))) return pci_number(0x7551);
    return nullptr;
}
kern_return_t lxt_IORegistryEntryGetRegistryEntryID(io_registry_entry_t, uint64_t *id) { *id = 7; return KERN_SUCCESS; }
kern_return_t lxt_IOServiceOpen(io_service_t service, task_port_t, uint32_t type, io_connect_t *connect)
{
    CHECK(service == 2 && (type == 0 || type == MLG_USER_CLIENT_OBSERVER || type == MLG_USER_CLIENT_LINUX_FILE));
    // MacLinuxGPU::NewUserClient: only observers while a session closes,
    // retires or is stuck.
    if (type != MLG_USER_CLIENT_OBSERVER &&
        (sessionFlagsNow.load() & (MLG_SESSION_FLAG_CLOSING | MLG_SESSION_FLAG_RETIRING)))
        return kIOReturnNotAttached;
    auto *ivars = new MacLinuxGPUUserClient_IVars{};
    ivars->sessionGeneration = 1;
    ivars->observer = type == MLG_USER_CLIENT_OBSERVER;
    ivars->linuxFile = type == MLG_USER_CLIENT_LINUX_FILE;
    ivars->ownerQueue = &ownerQueue;
    ivars->ownerLock = IOLockAlloc();
    auto *client = new MacLinuxGPUUserClient;
    client->ivars = ivars;
    std::lock_guard g(clientsLock);
    ivars->clientID = 10 + clients.size();
    *connect = ivars->linuxFile ? kConnection : nextConnection++;
    clients[*connect] = client;
    return KERN_SUCCESS;
}
kern_return_t lxt_IOServiceClose(io_connect_t) { return KERN_SUCCESS; }
kern_return_t lxt_IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
                                      uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
                                      uint64_t *output, uint32_t *outputCnt, void *outputStruct,
                                      size_t *outputStructCnt)
{
    ++syncCalls;
    return deliver(client_of(connection), selector, nullptr, input, inputCnt, inputStruct, inputStructCnt, output, outputCnt,
                   outputStruct, outputStructCnt);
}
kern_return_t lxt_IOConnectCallScalarMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
                                            uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt)
{
    return lxt_IOConnectCallMethod(connection, selector, input, inputCnt, nullptr, 0, output, outputCnt,
                                   nullptr, nullptr);
}
kern_return_t lxt_IOConnectCallAsyncMethod(mach_port_t connection, uint32_t selector, mach_port_t wake_port,
                                           uint64_t *reference, uint32_t referenceCnt, const uint64_t *input,
                                           uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
                                           uint64_t *output, uint32_t *outputCnt, void *outputStruct,
                                           size_t *outputStructCnt)
{
    CHECK(wake_port != MACH_PORT_NULL && referenceCnt >= kIOAsyncCalloutCount);
    MacLinuxGPUUserClient *client = client_of(connection);
    ++asyncCalls;
    auto *action = new OSAction;
    action->port = wake_port;
    action->callout = reference[kIOAsyncCalloutFuncIndex];
    action->refcon = reference[kIOAsyncCalloutRefconIndex];
    action->selector = selector;
    const kern_return_t kr = deliver(client, selector, action, input, inputCnt, inputStruct, inputStructCnt, output,
                                     outputCnt, outputStruct, outputStructCnt);
    // Answered, completed, and nothing kept the completion: none can come
    // (the client would wait for good, its Ping still answering).
    if (kr == kIOReturnSuccess && !action->completed && action->refs == 1) {
        char what[160];
        std::snprintf(what, sizeof(what), "selector %u was called async and the driver answered it on "
                      "the call: its completion can never come", selector);
        fail(what);
    }
    action->release();
    return kr;
}
IONotificationPortRef lxt_IONotificationPortCreate(mach_port_t)
{
    auto *p = new IONotificationPort;
    CHECK(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p->port) == KERN_SUCCESS);
    CHECK(mach_port_insert_right(mach_task_self(), p->port, p->port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    return p;
}
void lxt_IONotificationPortDestroy(IONotificationPortRef p)
{
    mach_port_mod_refs(mach_task_self(), p->port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), p->port);
    delete p;
}
mach_port_t lxt_IONotificationPortGetMachPort(IONotificationPortRef p) { return p->port; }
void lxt_IODispatchCalloutFromMessage(void *, mach_msg_header_t *msg, void *)
{
    const auto *m = reinterpret_cast<const CompletionMessage *>(msg);
    void *args[16];
    for (uint32_t i = 0; i < m->count; ++i) args[i] = (void *)(uintptr_t)m->args[i];
    reinterpret_cast<IOAsyncCallback>(m->callout)((void *)(uintptr_t)m->refcon, m->status, args, m->count);
}
// A Linux-file mapping anywhere; a BO at the address the client gives
// (its GPU VA in the host window, placed); anything else (the firmware
// mailbox) a zeroed page.
kern_return_t lxt_IOConnectMapMemory64(io_connect_t connection, uint32_t type, task_port_t,
                                       mach_vm_address_t *at, mach_vm_size_t *size, IOOptionBits options)
{
    uint64_t length = 16384, want = 0;
    if (connection == kConnection) {
        std::lock_guard g(mapLengthsLock);
        auto it = mapLengths.find(type);
        if (it == mapLengths.end()) return kIOReturnBadArgument;
        length = it->second;
    } else {
        client_of(connection);
        std::lock_guard g(sessionLock);
        auto it = boMaps.find(type);
        if (it != boMaps.end()) {
            length = it->second.second;
            want = it->second.first;
            if (!(options & kIOMapAnywhere) && *at != want) return kIOReturnBadArgument;
        }
    }
    void *p = mmap(want ? (void *)(uintptr_t)want : nullptr, length, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE | (want ? MAP_FIXED : 0), -1, 0);
    CHECK(p != MAP_FAILED);
    *at = (mach_vm_address_t)(uintptr_t)p;
    *size = length;
    return KERN_SUCCESS;
}
kern_return_t lxt_IOConnectUnmapMemory64(io_connect_t connection, uint32_t type, task_port_t, mach_vm_address_t at)
{
    uint64_t length = 16384;
    if (connection == kConnection) {
        std::lock_guard g(mapLengthsLock);
        auto it = mapLengths.find(type);
        if (it == mapLengths.end()) return kIOReturnBadArgument;
        length = it->second;
        mapLengths.erase(it);
    } else {
        std::lock_guard g(sessionLock);
        auto it = boMaps.find(type);
        if (it != boMaps.end()) length = it->second.second;
    }
    munmap((void *)(uintptr_t)at, length);
    return KERN_SUCCESS;
}
}

// ---- what a Vulkan client does ----
#define LINUX_IOC(dir, nr, size) (((dir) << 30) | ((size) << 16) | ('d' << 8) | (nr))
struct drm_get_cap_l { uint64_t capability, value; };
struct drm_amdgpu_info_l { uint64_t return_pointer; uint32_t return_size, query; uint32_t pad[4]; };
union drm_amdgpu_gem_create_l {
    struct { uint64_t bo_size, alignment, domains, domain_flags; } in;
    struct { uint32_t handle, pad; } out;
};
static const unsigned long kGetCap = LINUX_IOC(3u, 0x0c, sizeof(drm_get_cap_l));
static const unsigned long kAmdgpuInfo = LINUX_IOC(1u, 0x40 + 0x05, sizeof(drm_amdgpu_info_l));
static const unsigned long kGemCreate = LINUX_IOC(3u, 0x40 + 0x00, sizeof(drm_amdgpu_gem_create_l));

static void client_session(int fd, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        // Synchronous: a request that cannot sleep.
        drm_get_cap_l cap{0x10, 0};
        CHECK(mlg_ioctl(fd, kGetCap, &cap) == 0);
        CHECK(cap.value == 0xa0a0a0a0a0a0a0a0ull);
        // Async with the reply inline, then one long enough for LX_RESULT.
        drm_amdgpu_gem_create_l gem{};
        gem.in.bo_size = 65536;
        gem.in.domains = 4;
        CHECK(mlg_ioctl(fd, kGemCreate, &gem) == 7);
        CHECK(gem.out.handle == 0xa0a0a0a0u);
        std::vector<uint8_t> big(8192, 0);
        drm_amdgpu_info_l info{};
        info.return_pointer = (uint64_t)(uintptr_t)big.data();
        info.return_size = (uint32_t)big.size();
        info.query = 0x16;   // AMDGPU_INFO_DEV_INFO
        CHECK(mlg_ioctl(fd, kAmdgpuInfo, &info) == 7);
        CHECK(big[0] == big[8191] && big[0] >= 0xa0);
        // A BO map: MMAP, the mapping, LX_MMAP_COMMIT; then its unmap.
        void *p = mlg_mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x100000);
        CHECK(p != MAP_FAILED && p);
        memset(p, 0x5a, 65536);
        CHECK(mlg_munmap(p, 65536) == 0);
    }
}

// ---- what the HSA runtime does: LemonSeed Engine's hrx ----
static hsa_status_t find_gpu(hsa_agent_t agent, void *data)
{
    hsa_device_type_t type;
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS &&
        type == HSA_DEVICE_TYPE_GPU) {
        *static_cast<hsa_agent_t *>(data) = agent;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}
static void queue_error(hsa_status_t, hsa_queue_t *, void *) {}

static void hsa_session()
{
    // From a cold GPU: the runtime claims the session, places the host
    // window, initializes the GPU, then opens the compute session.
    CHECK(hsa_init() == HSA_STATUS_SUCCESS);
    hsa_agent_t gpu{};
    CHECK(hsa_iterate_agents(find_gpu, &gpu) == HSA_STATUS_INFO_BREAK && gpu.handle);
    CHECK(initDevices == 1 && gpuStage == 2);
    // hrx's device bring-up: several MULTI queues with an error callback.
    uint32_t slots = 0, maxSize = 0;
    CHECK(hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUE_MAX_SIZE, &maxSize) == HSA_STATUS_SUCCESS);
    hsa_queue_t *queues[3] = {};
    for (auto &q : queues) {
        const hsa_status_t status = hsa_queue_create(gpu, maxSize, HSA_QUEUE_TYPE_MULTI, queue_error, nullptr,
                                                     UINT32_MAX, UINT32_MAX, &q);
        if (status != HSA_STATUS_SUCCESS) {
            std::fprintf(stderr, "hsa_queue_create: %#x after %d queues (topology answers %d)\n", status,
                         queuesCreated.load(), topologyAnswers.load());
            fail("hsa_queue_create failed, as LemonSeed Engine's did on build 245");
        }
    }
    // The queue slots the driver reports (the topology's 16 words): what
    // the runtime lets this process create.
    CHECK(hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUES_MAX, &slots) == HSA_STATUS_SUCCESS);
    CHECK(slots == kQueueSlots);
    for (auto *q : queues) CHECK(hsa_queue_destroy(q) == HSA_STATUS_SUCCESS);
    // The device spec: the driver's structure, through OWNER_RESULT, in
    // mac_hsa.h's 32 words.
    mac_hsa_device_spec_t spec{};
    const hsa_status_t specStatus = mac_hsa_agent_get_device_spec(gpu, &spec, sizeof(spec));
    if (specStatus != HSA_STATUS_SUCCESS) {
        std::fprintf(stderr, "mac_hsa_agent_get_device_spec: %#x\n", specStatus);
        fail("the device spec was not answered");
    }
    CHECK(spec.words[0] == MLG_DEVICE_SPEC_VERSION && spec.words[1] == 4 && spec.words[2] == 2 &&
          spec.words[4] == 8 && spec.words[8] == 65536 && spec.words[9] == 64);
    CHECK(spec.words[10] == 0xff00 && spec.words[11] == 0xff01 && spec.words[17] == 0xff31);
    CHECK(spec.words[18] == 0xff && spec.words[19] == 0x12340000 && spec.words[20] == 0x56780000 &&
          spec.words[21] == 0xffff && spec.words[22] == 16 && spec.words[23] == 0 && spec.words[31] == 0);
    CHECK(hsa_shut_down() == HSA_STATUS_SUCCESS);
    CHECK(topologyAnswers >= 1 && queuesCreated >= 3);
}

// host/owner_call.h's every shape against the driver's owner calls: more
// scalars than a completion holds, with and without a structure output.
static void owner_call_shapes()
{
    io_connect_t connection = 0;
    CHECK(lxt_IOServiceOpen(2, mach_task_self(), 0, &connection) == KERN_SUCCESS);
    int state = 0;
    uint64_t out[16] = {};
    uint32_t n = 16;
    const uint64_t topology[1] = {10};
    CHECK(mlg_selector_call_on(connection, &state, kMacAMDGPUMethodQueryInfo, topology, 1, nullptr, 0, out, &n,
                               nullptr, nullptr) == kIOReturnSuccess);
    CHECK(n == 16 && out[0] == 1 && out[7] == kQueueSlots && out[14] == 1);
    const uint64_t test[1] = {kTestTag};
    uint8_t bytes[128] = {};
    size_t room = sizeof(bytes);
    n = 16;
    CHECK(mlg_selector_call_on(connection, &state, kMacAMDGPUMethodQueryInfo, test, 1, nullptr, 0, out, &n,
                               bytes, &room) == kIOReturnSuccess);
    CHECK(n == 14 && out[0] == 0x900 && out[13] == 0x90d && room == 100 && bytes[0] == 0x30 && bytes[99] == 0x93);
    // A caller that asks for fewer than a selector answers gets the driver's
    // refusal, as from the synchronous call.
    n = 12;
    CHECK(mlg_selector_call_on(connection, &state, kMacAMDGPUMethodQueryInfo, topology, 1, nullptr, 0, out, &n,
                               nullptr, nullptr) == kIOReturnBadArgument);
}

// A client that exits with an executable and a queue still loaded,
// as LemonSeed Engine does (build 246 aborted in exit(): the executable
// registry's static destructor freed GPU buffers through the transport's
// already-finalized mutex). Run as a process of its own; it must exit 0.
static int exit_loaded()
{
    CHECK(hsa_init() == HSA_STATUS_SUCCESS);
    hsa_agent_t gpu{};
    CHECK(hsa_iterate_agents(find_gpu, &gpu) == HSA_STATUS_INFO_BREAK && gpu.handle);
    mac_hsa::IsaTarget isa;
    CHECK(mac_hsa::resolveIsaTarget(120001, mac_hsa::TargetFeature::Off, mac_hsa::TargetFeature::Any, isa));
    mac_hsa::SignalKernelObjects objects;
    CHECK(mac_hsa::selectSignalKernels(isa, objects) && !objects.operations.empty());
    {
        hsa_code_object_reader_t reader{};
        hsa_executable_t executable{};
        CHECK(hsa_code_object_reader_create_from_memory(objects.operations.data(), objects.operations.size(),
                                                        &reader) == HSA_STATUS_SUCCESS);
        CHECK(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr,
                                        &executable) == HSA_STATUS_SUCCESS);
        const hsa_status_t loaded = hsa_executable_load_agent_code_object(executable, gpu, reader, nullptr, nullptr);
        if (loaded != HSA_STATUS_SUCCESS) std::fprintf(stderr, "load: %#x\n", loaded);
        CHECK(loaded == HSA_STATUS_SUCCESS);
        CHECK(hsa_executable_freeze(executable, nullptr) == HSA_STATUS_SUCCESS);
    }
    hsa_queue_t *queue = nullptr;
    CHECK(hsa_queue_create(gpu, 4096, HSA_QUEUE_TYPE_MULTI, queue_error, nullptr, UINT32_MAX, UINT32_MAX,
                           &queue) == HSA_STATUS_SUCCESS);
    std::printf("exit-loaded: an executable and a queue loaded; exiting without hsa_shut_down\n");
    std::fflush(stdout);
    return 0;  // exit() runs the static destructors
}

// hsa_init while the driver closes a session: the runtime waits, bounded,
// for the close, and names a session that will not close (build 246 said
// HSA_STATUS_ERROR_INVALID_AGENT).
static void close_waits()
{
    // A close that finishes: hsa_init waits for it.
    sessionFlagsNow = MLG_SESSION_FLAG_CLOSING;
    std::thread finish([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        sessionFlagsNow = 0;
    });
    const auto start = std::chrono::steady_clock::now();
    const hsa_status_t waited = hsa_init();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    finish.join();
    if (waited != HSA_STATUS_SUCCESS) std::fprintf(stderr, "hsa_init during a close: %#x\n", waited);
    CHECK(waited == HSA_STATUS_SUCCESS && ms >= 400);
    CHECK(hsa_shut_down() == HSA_STATUS_SUCCESS);

    // A close still running at the bound: busy, retry.
    setenv("MAC_HSA_SESSION_CLOSE_WAIT_MS", "300", 1);
    sessionFlagsNow = MLG_SESSION_FLAG_CLOSING;
    hsa_status_t status = hsa_init();
    CHECK(uint32_t(status) == uint32_t(HSA_STATUS_ERROR_RESOURCE_BUSY));
    const char *text = nullptr;
    CHECK(hsa_status_string(status, &text) == HSA_STATUS_SUCCESS && !strcmp(text, "HSA_STATUS_ERROR_RESOURCE_BUSY"));
    // A session that will not close: the device is lost, at once.
    sessionFlagsNow = MLG_SESSION_FLAG_CLOSING | MLG_SESSION_FLAG_QUARANTINED | MLG_SESSION_FLAG_RESTART_REQUIRED;
    status = hsa_init();
    CHECK(status == HSA_STATUS_ERROR_FATAL);
    // Retiring (an upgrade): busy.
    sessionFlagsNow = MLG_SESSION_FLAG_RETIRING;
    CHECK(uint32_t(hsa_init()) == uint32_t(HSA_STATUS_ERROR_RESOURCE_BUSY));
    sessionFlagsNow = 0;
    unsetenv("MAC_HSA_SESSION_CLOSE_WAIT_MS");
    std::printf("PASS hsa close: hsa_init waits for a closing session (%lld ms), reports a close past its "
                "bound and a retiring driver busy (HSA_STATUS_ERROR_RESOURCE_BUSY), a stuck session lost\n",
                (long long)ms);
}

int main(int argc, char **argv)
{
    ownerQueue.start();
    if (argc > 1 && !strcmp(argv[1], "exit-loaded")) {
        const int r = exit_loaded();
        // The kernel's own threads (the owner queue, workers) end with the
        // process, as a driver's do for an exiting client.
        return r;
    }
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!done) fail("a call never returned (60 s): a client waits for a completion that never comes");
    });

    // The HSA runtime from a cold GPU, as LemonSeed Engine brings it up.
    hsa_session();
    owner_call_shapes();
    close_waits();
    std::printf("PASS hsa transport: the HSA runtime's IOKit transport against the dext's session dispatch: "
                "cold bring-up with InitDevice, the 16-word topology through an owner call (%llu queue "
                "slots), three MULTI queues, the device spec as a structure; owner calls with 16 scalars, "
                "and 14 with a structure\n",
                (unsigned long long)kQueueSlots);

    // libmlg_drm from a cold GPU: the driver has no process for the client
    // and the GPU is not up; the library places the host window,
    // initializes it, and opens again.
    {
        std::lock_guard guard(sessionLock);
        go_cold();
    }
    hostWindows = 0;
    initDevices = 0;
    const int fd = mlg_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 3)
        std::fprintf(stderr, "first open: %d (Linux errno %d), host window calls %d, InitDevice %d\n", fd,
                     mlg_last_linux_errno(), hostWindows.load(), initDevices.load());
    CHECK(fd >= 3);
    CHECK(hostWindows == 2 && initDevices == 1 && sessionOpens >= 1 && opens == 1);

    // One client thread, then four at once.
    client_session(fd, 3);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([fd] { client_session(fd, 5); });
    for (auto &t : threads) t.join();
    CHECK(commits == 23 && mmaps == 23 && munmaps == 23);
    CHECK(syncIoctls == 23 && asyncIoctls == 46 && results == 23);

    // A selector the driver answers on the call, sent async (as build 243's
    // library sent LX_MMAP_COMMIT): refused at once, never left waiting.
    {
        uint64_t in[2] = {MLG_LX_MMAP_TYPE_BASE, 0x200000000ull}, out[1] = {};
        uint32_t n = 1;
        uint64_t ref[kIOAsyncCalloutCount] = {};
        IONotificationPortRef port = lxt_IONotificationPortCreate(MACH_PORT_NULL);
        CHECK(lxt_IOConnectCallAsyncMethod(kConnection, MLG_SELECTOR_LX_MMAP_COMMIT,
                                           lxt_IONotificationPortGetMachPort(port), ref, kIOAsyncCalloutCount,
                                           in, 2, nullptr, 0, out, &n, nullptr, nullptr) == kIOReturnBadArgument);
        lxt_IONotificationPortDestroy(port);
    }

    // LX_SCANOUT: synchronous, answered on the call.
    mlg_lx_scanout req{};
    mlg_lx_scanout_state state{};
    CHECK(mlg_scanout(&req, &state) != 0 && scanouts == 1);

    CHECK(mlg_close(fd) == 0 && closes == 1);
    done = true;
    watchdog.join();
    CHECK(ownerCallsOnDelivery == 0);
    for (auto &w : workers) w.join();
    ownerQueue.finish();
    std::printf("PASS lx transport: libmlg_drm's IOKit transport against the dext's Linux-file dispatch: "
                "first open with HostWindow and InitDevice, BO maps with LX_MMAP_COMMIT, LX_RESULT, LX_SCANOUT, "
                "four threads at once; %d sync and %d async calls in all, every async one completed\n",
                syncCalls.load(), asyncCalls.load());
    return 0;
}
