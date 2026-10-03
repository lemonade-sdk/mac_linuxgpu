/* DriverKit queue bridge for linuxu's pthread subset.  Each long-running
 * worker gets a separate serial queue so it cannot block the dext's bringup
 * or interrupt dispatch queues.  Queue resources are released after the
 * worker returns; pthread_join owns only the C task registry entry.
 */
#include <errno.h>
#include <stdint.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>

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
