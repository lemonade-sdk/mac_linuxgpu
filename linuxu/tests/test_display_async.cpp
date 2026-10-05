/* The dext's display calls with DriverKit mocked: the production
 * display_call machinery (extracted from dext/sources/MacLinuxGPUXcode.mm
 * by scripts/test-display-async.sh) against an OUTPUT that never finishes,
 * as one waiting for a hung GPU ring did on build 241 and froze every
 * client of the driver. An op that can sleep must not run on the call:
 * the call returns at once, PRESENT and other calls are answered while it
 * hangs, and its result arrives through the completion and RESULT. */
#include "observer_gate.h"
#include "session_state.h"
#include <rt/display.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <thread>
#include <vector>

using kern_return_t = int;
enum : int {
    kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnBusy = 2, kIOReturnNotReady = 3,
    kIOReturnNoResources = 4, kIOReturnNoMemory = 5, kIOReturnNotFound = 6, kIOReturnNoSpace = 7,
};
#define MACLINUXGPU_LOG(...) std::printf("log: " __VA_ARGS__), std::printf("\n")

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
struct IOMemoryDescriptor : OSObjectMock {};
struct OSAction : OSObjectMock {};
using IOUserClientAsyncArgumentsArray = uint64_t[16];
struct IOUserClientMethodArguments {
    const uint64_t *scalarInput;
    uint32_t scalarInputCount;
    OSData *structureInput;
    IOMemoryDescriptor *structureInputDescriptor;
    uint64_t *scalarOutput;
    uint32_t scalarOutputCount;
    OSData *structureOutput;
    IOMemoryDescriptor *structureOutputDescriptor;
    uint64_t structureOutputMaximumSize;
    OSAction *completion;
};
struct IOLock { std::mutex m; };
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *l) { delete l; }
static void IOLockLock(IOLock *l) { l->m.lock(); }
static void IOLockUnlock(IOLock *l) { l->m.unlock(); }
static void *IOMallocZero(size_t n) { return calloc(1, n); }
static void IOFree(void *p, size_t) { free(p); }

struct DisplayResultSlot;
struct ClientIVars { uint64_t displayToken; DisplayResultSlot *displayResults; };
struct Completion { uint64_t args[16]; uint32_t count; };
static std::mutex completionsLock;
static std::vector<Completion> completions;
struct MacLinuxGPUUserClient : OSObjectMock {
    ClientIVars *ivars;
    void AsyncCompletion(OSAction *, kern_return_t, const IOUserClientAsyncArgumentsArray data, uint32_t count) {
        Completion c{};
        memcpy(c.args, data, count * sizeof(uint64_t));
        c.count = count;
        std::lock_guard lock(completionsLock);
        completions.push_back(c);
    }
};

// ---- driver state and the operations the calls run ----
static mlg_observer_gate s_observerReads;
static uint32_t s_displayRunning;
static uint64_t s_displayOwner;
static IOLock *s_displayStopsLock;
static void *s_rtDevice;
static void *rt_device_get_pdev(void *) { return nullptr; }
static void displays_publish(struct pci_dev *, const struct rt_display_report &) {}
int rt_display_modes(struct pci_dev *, const char *, struct rt_display_modes *m) { memset(m, 0, sizeof(*m)); return 0; }
int rt_display_status(struct pci_dev *, struct rt_display_report *r) { memset(r, 0, sizeof(*r)); return 0; }
int rt_display_probe(struct pci_dev *, struct rt_display_report *r) { memset(r, 0, sizeof(*r)); return 0; }
int rt_display_show(struct pci_dev *, const char *, uint32_t, struct rt_display_report *r) { memset(r, 0, sizeof(*r)); return 0; }
int rt_display_off(struct pci_dev *, struct rt_display_report *r) { memset(r, 0, sizeof(*r)); return 0; }

// OUTPUT waits until released (a GPU that does not answer); PRESENT and
// the rest answer at once. Records which thread ran each op.
static std::mutex gpuLock;
static std::condition_variable gpuAnswer;
static bool gpuAnswers;
static std::thread::id callerThread;
static std::atomic<int> outputsRun, outputsOnCaller, presents;
static kern_return_t display_frames(uint64_t, IOUserClientMethodArguments *a, struct pci_dev *)
{
    a->scalarOutput[0] = 0;
    a->scalarOutput[1] = 0;
    a->scalarOutputCount = MLG_DISPLAY_WORDS;
    switch (a->scalarInput[0]) {
    case MLG_DISPLAY_OP_PRESENT:
        ++presents;
        return kIOReturnSuccess;
    case MLG_DISPLAY_OP_OUTPUT: {
        ++outputsRun;
        if (std::this_thread::get_id() == callerThread) ++outputsOnCaller;
        std::unique_lock lock(gpuLock);
        gpuAnswer.wait(lock, [] { return gpuAnswers; });
        struct rt_display_report report{};
        report.connectors = 1;
        a->structureOutput = OSData::withBytes(&report, sizeof(report));
        return kIOReturnSuccess;
    }
    case MLG_DISPLAY_OP_IMPORT:
        a->scalarOutput[1] = 7;	// a handle
        return kIOReturnSuccess;
    default:
        return kIOReturnSuccess;
    }
}

// The wait pool: a thread per job, or refused when told to.
static std::vector<std::thread> poolThreads;
static bool poolRefuses;
static int rt_wait_pool_run(void (*fn)(void *), void *arg)
{
    if (poolRefuses) return -11;
    poolThreads.emplace_back(fn, arg);
    return 0;
}
static std::vector<uint64_t> stoppedClients;
static void observer_display_client_stop_now(uint64_t clientID) { stoppedClients.push_back(clientID); }

#include "display_call_capture.h"
#include "display_async_production.inc"

// ---- the test ----
static MacLinuxGPUUserClient *newClient()
{
    auto *client = new MacLinuxGPUUserClient;
    client->ivars = new ClientIVars{0, display_slot_new()};
    return client;
}

static kern_return_t call(MacLinuxGPUUserClient *client, uint64_t op, uint64_t arg, OSAction *completion,
                          uint64_t out[2], OSData **output = nullptr, OSData *input = nullptr)
{
    const uint64_t in[3] = {op, arg, MLG_DISPLAY_CONFIRM};
    IOUserClientMethodArguments a{};
    a.scalarInput = in;
    a.scalarInputCount = 3;
    a.structureInput = input;
    a.scalarOutput = out;
    a.scalarOutputCount = MLG_DISPLAY_WORDS;
    a.structureOutputMaximumSize = 4096;
    a.completion = completion;
    const kern_return_t kr = display_call(client, 1, &a);
    if (output) *output = a.structureOutput;
    else if (a.structureOutput) a.structureOutput->release();
    return kr;
}

// The host app's own call (host/display_call.h through
// display_call_host.c), handed to the driver as the kernel hands it: the
// call's scalars, its structure input as data, its output capacity as
// structureOutputMaximumSize and its async reference as the completion.
// The mock call() above always offers 4096 bytes, which hid build 242's
// host passing none.
static kern_return_t hostCall(MacLinuxGPUUserClient *client, uint64_t op, uint64_t arg, const void *input,
                              size_t inputSize, OSAction *action, uint64_t out[2], size_t *capacity)
{
    display_call_capture cap{};
    display_call_host_capture(op, arg, input, inputSize, &cap);
    assert(cap.selector == MLG_SELECTOR_DISPLAY && cap.scalar_count == 3 && cap.has_completion);
    IOUserClientMethodArguments a{};
    a.scalarInput = cap.scalars;
    a.scalarInputCount = cap.scalar_count;
    OSData *data = cap.input_size ? OSData::withBytes(cap.input, cap.input_size) : nullptr;
    a.structureInput = data;
    a.scalarOutput = out;
    a.scalarOutputCount = cap.out_words;
    a.structureOutputMaximumSize = cap.has_output ? cap.output_capacity : 0;
    a.completion = action;
    *capacity = a.structureOutputMaximumSize;
    const kern_return_t kr = display_call(client, 1, &a);
    if (data) data->release();
    if (a.structureOutput) a.structureOutput->release();
    return kr;
}

static bool waitCompletions(size_t n)
{
    for (int i = 0; i < 2000; ++i) {
        { std::lock_guard lock(completionsLock); if (completions.size() >= n) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// An op run on the call would block it forever (the OUTPUT below never
// finishes until released): fail instead of hanging.
static void watchdog(int)
{
    static const char message[] = "FAIL display calls: a call did not return (an op that can sleep ran on it)\n";
    (void)!write(2, message, sizeof(message) - 1);
    _exit(1);
}

int main()
{
    std::signal(SIGALRM, watchdog);
    alarm(20);
    callerThread = std::this_thread::get_id();
    s_displayStopsLock = IOLockAlloc();
    s_observerReads.open();
    auto *client = newClient();
    auto *action = new OSAction;
    uint64_t out[2];
    struct mlg_display_output request{};
    strcpy(request.connector, "DP-4");
    request.width = 2560;
    request.height = 1440;
    OSData *outputRequest = OSData::withBytes(&request, sizeof(request));

    // An op that can sleep, called synchronously: refused, never run.
    assert(call(client, MLG_DISPLAY_OP_OUTPUT, 59950, nullptr, out, nullptr, outputRequest) == kIOReturnBadArgument);
    assert(outputsRun == 0);

    // With a completion: answered at once with a token; it runs elsewhere.
    const auto t0 = std::chrono::steady_clock::now();
    assert(call(client, MLG_DISPLAY_OP_OUTPUT, 59950, action, out, nullptr, outputRequest) == kIOReturnSuccess);
    assert(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
    const uint64_t token = out[1];
    assert(out[0] == 0 && token == 1);
    for (int i = 0; i < 1000 && outputsRun == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(outputsRun == 1 && outputsOnCaller == 0);

    // While it waits on the GPU: PRESENT is answered, another op is busy,
    // a second client is answered too, and a stop is put off.
    assert(call(client, MLG_DISPLAY_OP_PRESENT, 1, nullptr, out) == kIOReturnSuccess && presents == 1);
    assert(call(client, MLG_DISPLAY_OP_PROBE, 0, action, out) == kIOReturnBusy);
    auto *other = newClient();
    assert(call(other, MLG_DISPLAY_OP_PRESENT, 1, nullptr, out) == kIOReturnSuccess && presents == 2);
    observer_display_client_stop(99);
    assert(stoppedClients.empty());	// not while the op holds the display
    assert(call(client, MLG_DISPLAY_OP_RESULT, token, nullptr, out) == kIOReturnNotFound);

    // The GPU answers: the completion carries the token and the result,
    // the stop runs after, RESULT returns the report once.
    { std::lock_guard lock(gpuLock); gpuAnswers = true; }
    gpuAnswer.notify_all();
    assert(waitCompletions(1));
    for (auto &t : poolThreads) t.join();
    poolThreads.clear();
    {
        std::lock_guard lock(completionsLock);
        const Completion &c = completions[0];
        assert(c.count == MLG_DISPLAY_ASYNC_WORDS && c.args[0] == token && c.args[1] == kIOReturnSuccess &&
               c.args[4] == sizeof(struct rt_display_report));
    }
    assert(stoppedClients.size() == 1 && stoppedClients[0] == 99);
    OSData *report = nullptr;
    assert(call(client, MLG_DISPLAY_OP_RESULT, token, nullptr, out, &report) == kIOReturnSuccess);
    assert(report && report->getLength() == sizeof(struct rt_display_report));
    report->release();
    assert(call(client, MLG_DISPLAY_OP_RESULT, token, nullptr, out) == kIOReturnNotFound);
    assert(!s_displayRunning && s_observerReads.drained());

    // IMPORT's handle comes back in the completion.
    assert(call(client, MLG_DISPLAY_OP_IMPORT, (2560ull << 48) | (1440ull << 32) | 10240, action, out) == kIOReturnSuccess);
    assert(waitCompletions(2));
    for (auto &t : poolThreads) t.join();
    poolThreads.clear();
    { std::lock_guard lock(completionsLock); assert(completions[1].args[3] == 7 && completions[1].args[4] == 0); }

    // Every op that can sleep, as the host app starts it: accepted by the
    // driver's own validation and completed. Without the output capacity
    // the report ops are refused (what build 242's host met).
    {
        const char connector[] = "DP-4";
        struct {
            uint64_t op, arg;
            const void *input;
            size_t size;
        } const ops[] = {
            {MLG_DISPLAY_OP_PROBE, 0, nullptr, 0},
            {MLG_DISPLAY_OP_STATUS, 0, nullptr, 0},
            {MLG_DISPLAY_OP_MODES, 0, connector, sizeof(connector) - 1},
            {MLG_DISPLAY_OP_SHOW, 1, connector, sizeof(connector) - 1},
            {MLG_DISPLAY_OP_OFF, 0, nullptr, 0},
            {MLG_DISPLAY_OP_IMPORT, (2560ull << 48) | (1440ull << 32) | 10240, nullptr, 0},
            {MLG_DISPLAY_OP_OUTPUT, 59950, &request, sizeof(request)},
            {MLG_DISPLAY_OP_RELEASE, 7, nullptr, 0},
        };
        size_t done = 2;
        for (const auto &op : ops) {
            size_t capacity = 0;
            if (op.op == MLG_DISPLAY_OP_OUTPUT) {
                std::lock_guard lock(gpuLock);
                gpuAnswers = true;
            }
            const kern_return_t kr = hostCall(client, op.op, op.arg, op.input, op.size, action, out, &capacity);
            if (kr != kIOReturnSuccess) {
                std::fprintf(stderr, "FAIL display calls: the host's op %llu refused (%d), output capacity %zu\n",
                             (unsigned long long)op.op, kr, capacity);
                return 1;
            }
            assert(capacity >= sizeof(struct rt_display_report));
            assert(waitCompletions(++done));
            for (auto &t : poolThreads) t.join();
            poolThreads.clear();
            std::lock_guard lock(completionsLock);
            assert(completions[done - 1].args[0] == out[1] && completions[done - 1].args[1] == kIOReturnSuccess);
        }
        // The same call without an output capacity: refused before it runs.
        IOUserClientMethodArguments a{};
        const uint64_t in[3] = {MLG_DISPLAY_OP_STATUS, 0, MLG_DISPLAY_CONFIRM};
        a.scalarInput = in;
        a.scalarInputCount = 3;
        a.scalarOutput = out;
        a.scalarOutputCount = MLG_DISPLAY_WORDS;
        a.completion = action;
        assert(display_call(client, 1, &a) == kIOReturnBadArgument);
        assert(!s_displayRunning && s_observerReads.drained());
    }

    // No thread to run it on: refused, and the display is free again.
    poolRefuses = true;
    assert(call(client, MLG_DISPLAY_OP_PROBE, 0, action, out) == kIOReturnNoResources);
    assert(!s_displayRunning && s_observerReads.drained());
    poolRefuses = false;

    // A closed session admits nothing.
    s_observerReads.close();
    assert(call(client, MLG_DISPLAY_OP_PROBE, 0, action, out) == kIOReturnNotReady);
    assert(!s_displayRunning);

    // A client that stops after its op finished keeps the op's slot alive
    // only as long as the op needs it (ASan checks the frees).
    display_slot_put(client->ivars->displayResults);
    display_slot_put(other->ivars->displayResults);
    delete client->ivars;
    delete other->ivars;
    client->release();
    other->release();
    action->release();
    outputRequest->release();
    IOLockFree(s_displayStopsLock);
    std::puts("PASS display calls: an op that can sleep never runs on the call; PRESENT and other clients "
              "are answered while it hangs; completion, RESULT once, stops after it, no thread refused");
    return 0;
}
