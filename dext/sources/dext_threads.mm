/* DriverKit queue bridge for linuxu's pthread subset.  Each long-running
 * worker gets a separate serial queue so it cannot block the dext's bringup
 * or interrupt dispatch queues.  Queue resources are released after the
 * worker returns; pthread_join owns only the C task registry entry.
 */
#include <errno.h>
#include <stdint.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOTimerDispatchSource.h>

extern "C" int dext_thread_identity_swap(uint64_t identity,
                                          uint64_t *previous);
extern "C" void dext_thread_task_run(void *task);
extern "C" void dext_thread_task_fail(void *task);

struct DextThreadDispatch {
    void *task;
    uint64_t identity;
    IODispatchQueue *queue;
};

static void dext_thread_callback(void *opaque)
{
    auto *dispatch = static_cast<DextThreadDispatch *>(opaque);
    uint64_t previous = 0;
    if (dext_thread_identity_swap(dispatch->identity, &previous) == 0) {
        dext_thread_task_run(dispatch->task);
        uint64_t ignored = 0;
        (void)dext_thread_identity_swap(previous, &ignored);
    } else {
        dext_thread_task_fail(dispatch->task);
    }
    dispatch->queue->release();
    IOFree(dispatch, sizeof(*dispatch));
}

extern "C" int dext_thread_schedule(void *task, uint64_t identity)
{
    if (!task || !identity) return EINVAL;
    auto *dispatch = static_cast<DextThreadDispatch *>(
        IOMalloc(sizeof(DextThreadDispatch)));
    if (!dispatch) return ENOMEM;
    IODispatchQueue *queue = nullptr;
    kern_return_t ret = IODispatchQueue::Create("MacLinuxGPUWorker", 0, 0,
                                                 &queue);
    if (ret != kIOReturnSuccess || !queue) {
        IOFree(dispatch, sizeof(*dispatch));
        return EAGAIN;
    }
    dispatch->task = task;
    dispatch->identity = identity;
    dispatch->queue = queue;
    queue->DispatchAsync_f(dispatch, dext_thread_callback);
    return 0;
}

/* Blocking for linuxu's pthread condition variables (linuxu/src/shims/
 * dext_sync.c). DriverKit userspace has no condition variable, but a thread
 * running on a reentrant IODispatchQueue can Sleep on an event, which
 * releases the queue, until another thread on the queue Wakes it. The
 * queue serializes the waiter lists: a waiter registers itself, releases
 * the caller's mutex and sleeps on its own record; a signal takes waiters
 * off the list and wakes each record. Nothing polls. */
struct DextCondSleeper {
    DextCondSleeper *next;
    bool woken;
};

static IODispatchQueue *dext_cond_queue()
{
    static IODispatchQueue *queue;
    IODispatchQueue *current = __atomic_load_n(&queue, __ATOMIC_ACQUIRE);
    if (current) return current;
    IODispatchQueue *created = nullptr;
    if (IODispatchQueue::Create("MacLinuxGPUCond", kIODispatchQueueReentrant, 0,
                                &created) != kIOReturnSuccess || !created)
        return nullptr;
    IODispatchQueue *expected = nullptr;
    if (!__atomic_compare_exchange_n(&queue, &expected, created, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        created->release();
        return expected;
    }
    return created;
}

struct DextCondBlock {
    IODispatchQueue *queue;
    void **sleepers;
    bool (*still)(void *);
    void (*release)(void *);
    void *arg;
    uint64_t deadline_ns;
    int result;
};

static void dext_cond_block_on_queue(void *opaque)
{
    auto *b = static_cast<DextCondBlock *>(opaque);
    DextCondSleeper self = {static_cast<DextCondSleeper *>(*b->sleepers), false};
    *b->sleepers = &self;
    b->release(b->arg);
    while (!self.woken && b->still(b->arg)) {
        const kern_return_t kr = b->deadline_ns ?
            b->queue->SleepWithDeadline(&self, kIOTimerClockUptimeRaw, b->deadline_ns) :
            b->queue->SleepWithDeadline(&self, 0, 0);
        if (kr == kIOReturnTimeout) { b->result = ETIMEDOUT; break; }
        if (kr != kIOReturnSuccess) { b->result = EIO; break; }
    }
    if (!self.woken) {
        for (auto **link = reinterpret_cast<DextCondSleeper **>(b->sleepers); *link;
             link = &(*link)->next)
            if (*link == &self) { *link = self.next; break; }
    }
}

/* Register on @sleepers, @release (the caller's mutex) and sleep while
 * @still holds, until a dext_cond_wake or @deadline_ns (CLOCK_UPTIME_RAW,
 * 0 for none). 0 woken (or no longer still), ETIMEDOUT, EIO, or ENOTSUP
 * when no queue could be made (nothing was released then). */
extern "C" int dext_cond_block(void **sleepers, bool (*still)(void *),
                               void (*release)(void *), void *arg,
                               uint64_t deadline_ns)
{
    IODispatchQueue *queue = dext_cond_queue();
    if (!queue) return ENOTSUP;
    DextCondBlock block = {queue, sleepers, still, release, arg, deadline_ns, 0};
    queue->DispatchSync_f(&block, dext_cond_block_on_queue);
    return block.result;
}

struct DextCondWake {
    IODispatchQueue *queue;
    void **sleepers;
    bool all;
};

static void dext_cond_wake_on_queue(void *opaque)
{
    auto *w = static_cast<DextCondWake *>(opaque);
    while (*w->sleepers) {
        auto *sleeper = static_cast<DextCondSleeper *>(*w->sleepers);
        *w->sleepers = sleeper->next;
        sleeper->woken = true;
        (void)w->queue->Wakeup(sleeper);
        if (!w->all) break;
    }
}

/* Wake one or every sleeper of @sleepers. */
extern "C" int dext_cond_wake(void **sleepers, bool all)
{
    IODispatchQueue *queue = dext_cond_queue();
    if (!queue) return ENOTSUP;
    DextCondWake wake = {queue, sleepers, all};
    queue->DispatchSync_f(&wake, dext_cond_wake_on_queue);
    return 0;
}
