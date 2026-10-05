/* The dext's session calls with DriverKit mocked: the production owner_call
 * machinery (extracted from dext/sources/MacLinuxGPUXcode.mm by
 * scripts/test-owner-call.sh) against session selectors that sleep, as an
 * eviction or a probe waiting on a hung GPU ring did on build 241 and
 * froze every client of the driver. The delivery thread (the test's main
 * thread) must never wait: a call is queued for the owner's queue (a
 * thread of its own here) and answered through its completion; calls that
 * never sleep are answered at once; a synchronous call of one that can is
 * refused; a bounded read returns within its bound while its read hangs;
 * results come once through OWNER_RESULT; BO memory is found without the
 * session's state. */
#include "session_state.h"
#include "dext_compute.h"
#include <Block.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using kern_return_t = int;
enum : int {
    kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnBusy = 2, kIOReturnNotReady = 3,
    kIOReturnNoResources = 4, kIOReturnNoMemory = 5, kIOReturnNotFound = 6, kIOReturnNoSpace = 7,
    kIOReturnTimeout = 8, kIOReturnNotPermitted = 9, kIOReturnError = 10, kIOReturnUnsupported = 11,
};
static std::atomic<int> eventsLogged;
#define MACLINUXGPU_LOG(...) (std::printf("log: " __VA_ARGS__), std::printf("\n"))
#define MACLINUXGPU_EVENT(...) (++eventsLogged, std::printf("event: " __VA_ARGS__), std::printf("\n"))

// ---- DriverKit mocks ----
struct OSObjectMock {
    std::atomic<int> refs{1};
    virtual ~OSObjectMock() = default;
    void retain() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
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
struct IOMemoryDescriptor : OSObjectMock {
    uint64_t length = 0;
    kern_return_t GetLength(uint64_t *out) { *out = length; return kIOReturnSuccess; }
};
struct OSAction : OSObjectMock {};
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
struct IOLock { std::mutex m; };
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *l) { l->m.lock(); }
static void IOLockUnlock(IOLock *l) { l->m.unlock(); }
static void *IOMallocZero(size_t n) { return calloc(1, n); }
static void IOFree(void *p, size_t) { free(p); }

// The owner's queue: a thread of its own that runs what is queued, in order.
struct IODispatchQueue {
    using Block = void (^)(void);
    std::mutex lock;
    std::condition_variable kick;
    std::deque<Block> pending;
    bool stop = false;
    std::thread thread;
    std::thread::id id;
    void start() {
        thread = std::thread([this] {
            id = std::this_thread::get_id();
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
struct MacLinuxGPUUserClient_IVars {
    uint64_t clientID;
    IODispatchQueue *ownerQueue;
    IOLock *ownerLock;
    OwnerResult *ownerResults;
    uint64_t ownerToken;
    ClientMemory *memories;
    bool syncRefusalLogged;
};
struct Completion { uint64_t args[16]; uint32_t count; std::thread::id thread; };
static std::mutex completionsLock;
static std::condition_variable completionsChanged;
static std::vector<Completion> completions;
static std::thread::id deliveryThread;

// The session selectors as they run on the owner's queue: 16 (BOAlloc)
// answers three scalars, 50 (BORead) a structure, 9 (InitDevice) sleeps
// until the GPU answers, 87 (EVENT_WAIT) does not start.
static std::mutex gpuLock;
static std::condition_variable gpuAnswer;
static bool gpuAnswers;
static std::atomic<int> sessionCallsOnDelivery, sessionCallsRun;
struct MacLinuxGPUUserClient : OSObjectMock {
    MacLinuxGPUUserClient_IVars *ivars;
    void AsyncCompletion(OSAction *, kern_return_t, const IOUserClientAsyncArgumentsArray data, uint32_t count) {
        Completion c{};
        memcpy(c.args, data, count * sizeof(uint64_t));
        c.count = count;
        c.thread = std::this_thread::get_id();
        std::lock_guard lock(completionsLock);
        completions.push_back(c);
        completionsChanged.notify_all();
    }
    kern_return_t ExternalMethod(uint64_t selector, IOUserClientMethodArguments *a, const void *,
                                 void *, void *reference);
};

// ---- the state the direct calls read ----
static bool s_sessionClosing;
static bool s_probeAttempted = true, s_modulesRunning = true;
static int s_probeResult;
static int dext_pci_transport_fault() { return 0; }
static uint64_t dext_pci_transport_fault_offset() { return 0; }
static std::atomic<int> powerWaits;
static kern_return_t power_wait(MacLinuxGPUUserClient *, OSAction *, uint64_t) { ++powerWaits; return kIOReturnSuccess; }
static void power_snapshot(uint64_t *out) { memset(out, 0, MLG_POWER_STATE_WORDS * 8); out[0] = MLG_POWER_STATE_VERSION; }
static void session_state(uint64_t *out) { memset(out, 0, MLG_SESSION_STATE_WORDS * 8); out[0] = MLG_SESSION_STATE_VERSION; }
static size_t klog_read(uint64_t *cursor, char *out, size_t capacity, uint64_t *end)
{
    static const char text[] = "probe completed";
    size_t n = sizeof(text) - 1 < capacity ? sizeof(text) - 1 : capacity;
    memcpy(out, text, n);
    *cursor += n;
    *end = n;
    return n;
}
int dext_compute_runtime_build_cached(uint64_t *out) { out[0] = 1; out[1] = 1; out[2] = 243; out[3] = 243; return 0; }

// SysfsRead hangs until the GPU answers (an SMU message that never returns).
static std::mutex smuLock;
static std::condition_variable smuAnswer;
static bool smuAnswers;
static std::atomic<int> readsRun;
static kern_return_t observer_sysfs_read(IOUserClientMethodArguments *a)
{
    ++readsRun;
    std::unique_lock lock(smuLock);
    smuAnswer.wait(lock, [] { return smuAnswers; });
    a->scalarOutput[0] = 0; a->scalarOutput[1] = 3; a->scalarOutput[2] = 3;
    a->scalarOutputCount = MLG_SYSFS_READ_WORDS;
    a->structureOutput = OSData::withBytes("42\n", 3);
    return kIOReturnSuccess;
}
static kern_return_t observer_drm_info(IOUserClientMethodArguments *a) { return observer_sysfs_read(a); }

// BO memory for the mapping table.
static IOMemoryDescriptor *lastDescriptor;
int dext_compute_bo_memory(uint32_t type, void **cpu, uint64_t *size)
{
    if (type != 0x10001) return -ENOENT_L;
    *cpu = (void *)0x1000; *size = 4096;
    return 0;
}
static IOMemoryDescriptor *copy_bo_ranges_descriptor(uint32_t, uint64_t) { return nullptr; }
static void *dext_dma_copy_descriptor(void *) { lastDescriptor = new IOMemoryDescriptor; return lastDescriptor; }

// rt/bounded.h, as linuxu/src/amdgpu-rt/bounded.c behaves (its own test
// covers it): run on a thread, wait up to @ms.
struct BoundedRun {
    std::mutex lock;
    std::condition_variable done;
    bool finished = false, abandoned = false;
};
static std::vector<std::thread> boundedThreads;
extern "C" int rt_bounded_run(void (*fn)(void *), void *arg, void (*release)(void *), unsigned int ms)
{
    auto *run = new BoundedRun;
    boundedThreads.emplace_back([=] {
        fn(arg);
        bool abandoned;
        { std::lock_guard guard(run->lock); run->finished = true; abandoned = run->abandoned; run->done.notify_all(); }
        if (abandoned) { release(arg); delete run; }
    });
    std::unique_lock guard(run->lock);
    if (!run->done.wait_for(guard, std::chrono::milliseconds(ms), [&] { return run->finished; })) {
        run->abandoned = true;
        return -110;
    }
    guard.unlock();
    delete run;
    return 0;
}

#include "owner_call_production.inc"

kern_return_t MacLinuxGPUUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments *a,
                                                const void *, void *, void *reference)
{
    assert(reference == &kOwnerJob);
    ++sessionCallsRun;
    if (std::this_thread::get_id() == deliveryThread) ++sessionCallsOnDelivery;
    switch (selector) {
    case 16:
        assert(a->scalarInputCount == 4 && a->scalarInput[0] == 4096 && a->scalarOutputCount >= 3);
        a->scalarOutput[0] = 0x10001; a->scalarOutput[1] = 0x400000; a->scalarOutput[2] = 0;
        a->scalarOutputCount = 3;
        return kIOReturnSuccess;
    case 50: {
        uint8_t bytes[64];
        for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)i;
        assert(a->structureOutputMaximumSize >= sizeof(bytes));
        a->structureOutput = OSData::withBytes(bytes, sizeof(bytes));
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    case 9: {
        std::unique_lock lock(gpuLock);
        gpuAnswer.wait(lock, [] { return gpuAnswers; });
        a->scalarOutputCount = 0;
        return kIOReturnSuccess;
    }
    case MLG_SELECTOR_EVENT_WAIT:
        assert(a->completion);
        a->scalarOutput[0] = (uint64_t)(int64_t)-11;  // every waiting thread busy
        a->scalarOutputCount = 1;
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}


// ---- the checks ----
static Completion waitCompletion(size_t index)
{
    std::unique_lock lock(completionsLock);
    assert(completionsChanged.wait_for(lock, std::chrono::seconds(5), [&] { return completions.size() > index; }));
    return completions[index];
}

static double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main()
{
    deliveryThread = std::this_thread::get_id();
    IODispatchQueue owner;
    owner.start();
    MacLinuxGPUUserClient client;
    MacLinuxGPUUserClient_IVars ivars{};
    ivars.clientID = 1;
    ivars.ownerQueue = &owner;
    ivars.ownerLock = IOLockAlloc();
    client.ivars = &ivars;

    // A session call that sleeps (a probe waiting on a hung GPU): the
    // delivery thread queues it and returns at once.
    OSAction *action = new OSAction;
    uint64_t out[16] = {};
    IOUserClientMethodArguments a{};
    a.completion = action;
    a.scalarOutput = out;
    a.scalarOutputCount = 1;
    auto start = std::chrono::steady_clock::now();
    assert(owner_call(&client, 9, &a) == kIOReturnSuccess);
    assert(msSince(start) < 50 && a.scalarOutputCount == 1 && out[0] == 1);  // its token

    // Everything else on the delivery thread still answers while it sleeps:
    // the cached queries at once, more session calls queued behind it.
    uint64_t ping[1] = {};
    IOUserClientMethodArguments p{};
    p.scalarOutput = ping; p.scalarOutputCount = 1;
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_PING, nullptr, 0));
    assert(direct_call(&client, MLG_SELECTOR_PING, &p) == kIOReturnSuccess && ping[0] == 0xA117AB1Eu);
    uint64_t build[4] = {};
    IOUserClientMethodArguments b{};
    b.scalarOutput = build; b.scalarOutputCount = 4;
    assert(direct_call(&client, MLG_SELECTOR_RUNTIME_BUILD, &b) == kIOReturnSuccess && build[2] == 243);
    const uint64_t klogTag[2] = {DEXT_COMPUTE_QUERY_KERNEL_LOG, 0};
    uint64_t klog[16] = {};
    IOUserClientMethodArguments k{};
    k.scalarInput = klogTag; k.scalarInputCount = 2; k.scalarOutput = klog; k.scalarOutputCount = 16;
    assert(mlg_call_runs_on_delivery(MLG_SELECTOR_QUERY_INFO, klogTag, 2));
    assert(direct_call(&client, MLG_SELECTOR_QUERY_INFO, &k) == kIOReturnSuccess && klog[2] == 15);
    const uint64_t waitIn[2] = {MLG_POWER_OP_WAIT, 3};
    IOUserClientMethodArguments w{};
    w.scalarInput = waitIn; w.scalarInputCount = 2; w.completion = action;
    assert(direct_call(&client, MLG_SELECTOR_POWER, &w) == kIOReturnSuccess && powerWaits == 1);

    const uint64_t allocIn[4] = {4096, 2, 4096, 0};
    uint64_t allocOut[3] = {};
    IOUserClientMethodArguments alloc{};
    alloc.completion = action; alloc.scalarInput = allocIn; alloc.scalarInputCount = 4;
    alloc.scalarOutput = allocOut; alloc.scalarOutputCount = 3;
    start = std::chrono::steady_clock::now();
    assert(owner_call(&client, 16, &alloc) == kIOReturnSuccess && allocOut[0] == 2);
    const uint64_t readIn[3] = {0x10001, 0, 64};
    IOUserClientMethodArguments read{};
    read.completion = action; read.scalarInput = readIn; read.scalarInputCount = 3;
    read.structureOutputMaximumSize = 64;
    assert(owner_call(&client, 50, &read) == kIOReturnSuccess);
    const uint64_t waitArgs[4] = {77, 1, 0, 100};
    uint32_t ids[1] = {5};
    OSData *idData = OSData::withBytes(ids, sizeof(ids));
    uint64_t waitOut[1] = {99};
    IOUserClientMethodArguments ev{};
    ev.completion = action; ev.scalarInput = waitArgs; ev.scalarInputCount = 4;
    ev.structureInput = idData; ev.scalarOutput = waitOut; ev.scalarOutputCount = 1;
    assert(owner_call(&client, MLG_SELECTOR_EVENT_WAIT, &ev) == kIOReturnSuccess && waitOut[0] == 0);
    idData->release();
    assert(msSince(start) < 50);
    {
        std::lock_guard lock(completionsLock);
        assert(completions.empty());  // all behind the sleeping probe, in order
    }

    // A synchronous call of a session selector: refused, logged once.
    assert(refuse_sync(&client, 16) == kIOReturnNotPermitted);
    assert(refuse_sync(&client, 17) == kIOReturnNotPermitted && eventsLogged == 1);

    // The GPU answers: the calls complete in order, on the owner's queue.
    { std::lock_guard lock(gpuLock); gpuAnswers = true; gpuAnswer.notify_all(); }
    Completion init = waitCompletion(0), allocated = waitCompletion(1), readDone = waitCompletion(2),
               eventDone = waitCompletion(3);
    assert(init.thread == owner.id && init.args[0] == 1 && init.args[1] == kIOReturnSuccess &&
           init.args[2] == 0 && init.args[3] == 0 && init.count == MLG_OWNER_ASYNC_HEADER);
    assert(allocated.args[0] == 2 && allocated.args[1] == kIOReturnSuccess && allocated.args[2] == 3 &&
           allocated.args[4] == 0x10001 && allocated.args[5] == 0x400000 &&
           allocated.count == MLG_OWNER_ASYNC_HEADER + 3);
    assert(readDone.args[0] == 3 && readDone.args[3] == 64);
    // EVENT_WAIT that did not start: its own layout, with the reason.
    assert(eventDone.count == MLG_EVENT_WAIT_WORDS && eventDone.args[0] == 77 &&
           (int64_t)eventDone.args[1] == -11 && eventDone.args[2] == 2);
    assert(sessionCallsRun == 4 && sessionCallsOnDelivery == 0);

    // The structure output, once, with room; short room keeps it.
    const uint64_t token[1] = {3};
    IOUserClientMethodArguments fetch{};
    fetch.scalarInput = token; fetch.scalarInputCount = 1; fetch.structureOutputMaximumSize = 8;
    assert(owner_result(&client, &fetch) == kIOReturnNoSpace);
    fetch.structureOutputMaximumSize = 64;
    assert(owner_result(&client, &fetch) == kIOReturnSuccess && fetch.structureOutput &&
           fetch.structureOutput->getLength() == 64 &&
           static_cast<const uint8_t *>(fetch.structureOutput->getBytesNoCopy())[63] == 63);
    fetch.structureOutput->release();
    fetch.structureOutput = nullptr;
    assert(owner_result(&client, &fetch) == kIOReturnNotFound);

    // BO memory: made on the owner's queue at BOMap, found on the delivery
    // thread without the session's state, gone at BOFree.
    client_memory_stash(&client, 0x77, 0x10001);
    IOMemoryDescriptor *found = client_memory_find(&client, 0x10001);
    assert(found && found == lastDescriptor && found->refs == 2);
    found->release();
    assert(!client_memory_find(&client, 0x10002));
    client_memory_drop(&client, 0x77);
    assert(!client_memory_find(&client, 0x10001));

    // A bounded read whose SMU message never returns: answered with a
    // timeout within its bound; the next is busy until it ends.
    const uint64_t sysfsIn[2] = {MLG_SYSFS_OP_READ, 0};
    OSData *path = OSData::withBytes("gpu_busy_percent", 16);
    uint64_t sysfsOut[3] = {};
    IOUserClientMethodArguments sysfs{};
    sysfs.scalarInput = sysfsIn; sysfs.scalarInputCount = 2; sysfs.structureInput = path;
    sysfs.scalarOutput = sysfsOut; sysfs.scalarOutputCount = 3; sysfs.structureOutputMaximumSize = 4096;
    start = std::chrono::steady_clock::now();
    assert(bounded_read(MLG_SELECTOR_SYSFS_READ, &sysfs) == kIOReturnTimeout);
    const double waited = msSince(start);
    assert(waited >= MLG_BOUNDED_READ_MS - 5 && waited < MLG_BOUNDED_READ_MS + 200);
    assert(bounded_read(MLG_SELECTOR_SYSFS_READ, &sysfs) == kIOReturnBusy && readsRun == 1);
    { std::lock_guard lock(smuLock); smuAnswers = true; smuAnswer.notify_all(); }
    for (auto &thread : boundedThreads) thread.join();
    boundedThreads.clear();
    // Answered now: within the bound, with its output.
    sysfs.scalarOutputCount = 3;
    assert(bounded_read(MLG_SELECTOR_SYSFS_READ, &sysfs) == kIOReturnSuccess &&
           sysfs.structureOutput && sysfs.structureOutput->getLength() == 3 && sysfsOut[1] == 3);
    sysfs.structureOutput->release();
    for (auto &thread : boundedThreads) thread.join();
    path->release();

    owner_results_free(&client);
    owner.finish();
    action->release();
    std::printf("PASS session calls: a session call that sleeps never holds the delivery thread; "
                "cached queries and the power wait answer at once, later calls queue in order, a "
                "synchronous call of one is refused, completions carry scalars and EVENT_WAIT's own "
                "layout, RESULT once, BO memory found without the session, a hung bounded read "
                "times out within %u ms and makes the next busy\n", MLG_BOUNDED_READ_MS);
    return 0;
}
