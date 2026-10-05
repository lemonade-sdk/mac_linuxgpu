/* libmlg_drm's real IOKit transport (libmlg_drm/src/mlg_transport_iokit.c,
 * mlg_drm.c, mlg_init.c and host/selector_call.h, unchanged) against the
 * dext's real dispatch of a Linux-file client (dext/sources/
 * MacLinuxGPUXcode.mm: ExternalMethod's delivery part, lx_external_method,
 * the owner calls; extracted by scripts/test-lx-transport.sh), with only
 * the Linux process behind it (rt_lx_*) and DriverKit mocked.
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
    std::fprintf(stderr, "FAIL lx transport: %s\n", what);
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
struct MacLinuxGPUUserClient_IVars {
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
static void session_state(uint64_t *out) { memset(out, 0, MLG_SESSION_STATE_WORDS * 8); out[0] = MLG_SESSION_STATE_VERSION; }
static size_t klog_read(uint64_t *, char *, size_t, uint64_t *end) { *end = 0; return 0; }
int dext_compute_runtime_build_cached(uint64_t *out)
{
    out[0] = 1; out[1] = 1; out[2] = MLG_SESSION_CALLS_ASYNC_BUILD; out[3] = MLG_SESSION_CALLS_ASYNC_BUILD;
    return 0;
}
static kern_return_t observer_sysfs_read(IOUserClientMethodArguments *) { return kIOReturnUnsupported; }
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
static std::vector<std::thread> workers;
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
#include "lx_transport_production.inc"

// The session calls a Linux-file client makes on the owner's queue (its
// part of ExternalMethod): the GPU's host window and its initialization.
static kern_return_t owner_session_call(uint64_t selector, IOUserClientMethodArguments *a)
{
    if (std::this_thread::get_id() == deliveryThread) ++ownerCallsOnDelivery;
    switch (selector) {
    case kMacAMDGPUMethodHostWindow: {
        static uint64_t base;
        ++hostWindows;
        CHECK(a->scalarInputCount == 1 && a->scalarOutputCount >= 3);
        if (a->scalarInput[0]) base = a->scalarInput[0];
        a->scalarOutput[0] = base;
        a->scalarOutput[1] = 1ull << 30;
        a->scalarOutput[2] = 0;
        a->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }
    case kMacAMDGPUMethodInitDevice:
        ++initDevices;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the probe
        s_modulesRunning = true;
        s_probeAttempted = true;
        s_rtDevice = (void *)0x1000;
        s_lxCalls.open();
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    default:
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
    return owner_session_call(selector, arguments);
}

// ---- the test kernel: IOKit for the library ----
static IODispatchQueue ownerQueue;
static MacLinuxGPUUserClient *dextClient;
static std::mutex delivery;         // DriverKit delivers a driver's calls on one thread
static const io_connect_t kConnection = 0x1234;
static std::atomic<int> asyncCalls, syncCalls;

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
static kern_return_t deliver(uint32_t selector, OSAction *action, const uint64_t *input, uint32_t inputCnt,
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
    a.scalarOutputCount = outputCnt ? *outputCnt : 0;
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
        kr = dextClient->ExternalMethod(selector, &a, nullptr, nullptr, nullptr);
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
kern_return_t lxt_IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef matching, io_iterator_t *it)
{
    if (matching) CFRelease(matching);
    *it = 1;
    return KERN_SUCCESS;
}
static int iteratorLeft;
io_object_t lxt_IOIteratorNext(io_iterator_t)
{
    return iteratorLeft-- > 0 ? 2 : 0;
}
kern_return_t lxt_IOObjectRelease(io_object_t) { return KERN_SUCCESS; }
CFTypeRef lxt_IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef,
                                              IOOptionBits)
{
    if (entry == 2 && CFStringCompare(key, CFSTR("IOUserClass"), 0) == kCFCompareEqualTo)
        return CFRetain(CFSTR("MacLinuxGPU"));
    return nullptr;
}
kern_return_t lxt_IORegistryEntryGetRegistryEntryID(io_registry_entry_t, uint64_t *id) { *id = 7; return KERN_SUCCESS; }
kern_return_t lxt_IOServiceOpen(io_service_t service, task_port_t, uint32_t type, io_connect_t *connect)
{
    CHECK(service == 2 && type == MLG_USER_CLIENT_LINUX_FILE);
    *connect = kConnection;
    return KERN_SUCCESS;
}
kern_return_t lxt_IOServiceClose(io_connect_t) { return KERN_SUCCESS; }
kern_return_t lxt_IOConnectCallMethod(mach_port_t connection, uint32_t selector, const uint64_t *input,
                                      uint32_t inputCnt, const void *inputStruct, size_t inputStructCnt,
                                      uint64_t *output, uint32_t *outputCnt, void *outputStruct,
                                      size_t *outputStructCnt)
{
    CHECK(connection == kConnection);
    ++syncCalls;
    return deliver(selector, nullptr, input, inputCnt, inputStruct, inputStructCnt, output, outputCnt,
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
    CHECK(connection == kConnection && wake_port != MACH_PORT_NULL && referenceCnt >= kIOAsyncCalloutCount);
    ++asyncCalls;
    auto *action = new OSAction;
    action->port = wake_port;
    action->callout = reference[kIOAsyncCalloutFuncIndex];
    action->refcon = reference[kIOAsyncCalloutRefconIndex];
    action->selector = selector;
    const kern_return_t kr = deliver(selector, action, input, inputCnt, inputStruct, inputStructCnt, output,
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
kern_return_t lxt_IOConnectMapMemory64(io_connect_t connection, uint32_t type, task_port_t,
                                       mach_vm_address_t *at, mach_vm_size_t *size, IOOptionBits)
{
    CHECK(connection == kConnection);
    uint64_t length;
    {
        std::lock_guard g(mapLengthsLock);
        auto it = mapLengths.find(type);
        if (it == mapLengths.end()) return kIOReturnBadArgument;
        length = it->second;
    }
    void *p = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(p != MAP_FAILED);
    *at = (mach_vm_address_t)(uintptr_t)p;
    *size = length;
    return KERN_SUCCESS;
}
kern_return_t lxt_IOConnectUnmapMemory64(io_connect_t connection, uint32_t type, task_port_t, mach_vm_address_t at)
{
    CHECK(connection == kConnection);
    std::lock_guard g(mapLengthsLock);
    auto it = mapLengths.find(type);
    if (it == mapLengths.end()) return kIOReturnBadArgument;
    munmap((void *)(uintptr_t)at, it->second);
    mapLengths.erase(it);
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

int main()
{
    // The driver as a Linux-file client finds it: attached, the GPU not up.
    ownerQueue.start();
    auto *ivars = new MacLinuxGPUUserClient_IVars{};
    ivars->sessionGeneration = 1;
    ivars->clientID = 11;
    ivars->linuxFile = true;
    ivars->ownerQueue = &ownerQueue;
    ivars->ownerLock = IOLockAlloc();
    dextClient = new MacLinuxGPUUserClient;
    dextClient->ivars = ivars;
    iteratorLeft = 1;

    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 600 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!done) fail("a call never returned (30 s): the library waits for a completion that never comes");
    });

    // The first open: the driver has no process for the client and the GPU
    // is not initialized; the library places the host window, initializes
    // it, and opens again.
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
                "first open with HostWindow and InitDevice, %d sync and %d async calls, every async one "
                "completed, BO maps with LX_MMAP_COMMIT, LX_RESULT, LX_SCANOUT, four threads at once\n",
                syncCalls.load(), asyncCalls.load());
    return 0;
}
