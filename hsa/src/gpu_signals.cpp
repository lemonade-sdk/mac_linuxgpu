#include "runtime_state.h"
#include "code_object.h"
#include "signal_kernels.h"
#include "synchronization_policy.h"
#include "gpu_signal_service.h"
#include <array>
#include <map>
#include <system_error>
#include <cstdio>

namespace mac_hsa::detail {
namespace {
struct GPUSignalContext {
    std::shared_ptr<Connection> connection;
    DeviceBuffer code,arguments;
    SharedBuffer result,arena;
    SignalKernelObjects kernels;
    CodeObject object;
    std::unique_ptr<GPUSignalService> service;
    std::mutex slotsMutex,operationsMutex;
    std::array<bool,256> used{};
    std::atomic<bool> faulted{false};
    std::array<std::weak_ptr<Signal>,256> signals;
    explicit GPUSignalContext(std::shared_ptr<Connection> c):connection(std::move(c)) {}
    ~GPUSignalContext() {
        if (service && !service->shutdown()) return; // retain arena/code if retirement is uncertain
        if (arguments.handle) connection->freeBuffer(arguments);
        if (code.handle) connection->freeBuffer(code);
        if (result.host) connection->freeSharedBuffer(result);
        if (arena.host) connection->freeSharedBuffer(arena);
    }
    hsa_status_t initialize() {
        DeviceSnapshot snapshot;
        auto status=connection->read(snapshot);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!(synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot) &
              MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        IsaTarget isa;
        if (!deviceIsa(snapshot,isa)) return HSA_STATUS_ERROR_INVALID_ISA;
        std::string error;
        if (!selectSignalKernels(isa,kernels,&error)) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true))
                std::fprintf(stderr,"mac_hsa: GPU signals unavailable: %s\n",error.c_str());
            return HSA_STATUS_ERROR_INVALID_ISA;
        }
        if (!parseCodeObject(kernels.operations,object,isa) || object.kernels.size()!=1)
            return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        const auto &kernel=object.kernels[0];
        if (kernel.kernargSize!=36 || !signalKernelProperties(kernel.properties) ||
            kernel.privateSize || kernel.groupSize || kernel.preload)
            return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status=connection->allocateBuffer(object.image.size(),code);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!relocateCodeObject(object,code.address)) return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
        status=connection->writeBuffer(code,0,object.image.data(),object.image.size());
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateBuffer(16384,arguments);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,result);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,arena);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!arena.host || arena.device.size<16384 || arena.device.address!=reinterpret_cast<uintptr_t>(arena.host) ||
            !result.host || result.device.size<8 || result.device.address!=reinterpret_cast<uintptr_t>(result.host))
            return HSA_STATUS_ERROR;
        const char *backend=std::getenv("MAC_HSA_SIGNAL_BACKEND");
        if (useSignalMailbox(MemoryPath::DriverKitShared,snapshot,backend))
            try {service=std::make_unique<GPUSignalService>(connection,arena,isa,kernels.mailbox);}
            catch (const std::system_error &) { /* Worker unavailable: keep bounded one-shot executor. */ }
        return HSA_STATUS_SUCCESS;
    }
    bool fail() {
        std::array<std::shared_ptr<Signal>,256> affected;
        {
            std::lock_guard lock(slotsMutex);
            faulted=true;
            for (unsigned i=0;i<signals.size();++i) affected[i]=signals[i].lock();
        }
        // Release strong references outside slotsMutex: a signal's final
        // destructor returns its slot under that same mutex.
        for (auto &signal:affected) if (signal) {
            signal->alive=false;signal->changed.notify_all();
        }
        return false;
    }
    // CPU-polled fallback for when no queue slot is free for the GPU
    // executor. The operation runs as a CPU atomic on the shared word: the
    // DriverKit mapping is qualified for CPU-local atomics and ordered
    // ownership transfer (MAC_HSA_SYNC_OWNERSHIP_TRANSFER), so it is exact
    // whenever no GPU agent modifies the same signal concurrently (CPU
    // stores/resets of completion and barrier signals, CPU-side waits). It
    // does not provide simultaneous CPU/GPU read-modify-write on one word.
    bool cpuFallbackReported=false;
    bool executeOnCPU(unsigned slot,unsigned operation,int64_t value,int64_t compare,int64_t &old) {
        auto *abi=static_cast<SignalABI *>(arena.host)+slot;
        std::atomic_ref<int64_t> word(abi->value);
        switch (operation) {
        case 1: case 7: old=word.exchange(value,std::memory_order_seq_cst);break;
        case 2: old=word.fetch_add(value,std::memory_order_seq_cst);break;
        case 3: old=word.fetch_sub(value,std::memory_order_seq_cst);break;
        case 4: old=word.fetch_and(value,std::memory_order_seq_cst);break;
        case 5: old=word.fetch_or(value,std::memory_order_seq_cst);break;
        case 6: old=word.fetch_xor(value,std::memory_order_seq_cst);break;
        case 8: old=compare;word.compare_exchange_strong(old,value,std::memory_order_seq_cst);break;
        default: return false;
        }
        const auto *trace=std::getenv("MAC_HSA_SIGNAL_TRACE");
        if (!cpuFallbackReported && trace && std::strcmp(trace,"1")==0)
            std::fprintf(stderr,"signal-mailbox: backend=cpu reason=queue-slots-held\n");
        cpuFallbackReported=true;
        return true;
    }
    bool execute(unsigned slot,unsigned operation,int64_t value,int64_t compare,int64_t &old) {
        std::lock_guard lock(operationsMutex);
        if (faulted || operation<1 || operation>8) return false;
        if (service) {
            const auto status=service->execute(slot,operation,value,compare,old);
            if (status==SignalServiceResult::Success) return true;
            if (status==SignalServiceResult::Failed) return fail();
            // Unavailable means no request was published: every queue slot the
            // driver reports is held by an application queue (with a single
            // slot, any public queue). The bounded one-shot dispatch below runs
            // the same operation on demand and needs no persistent queue.
        }
        const uint64_t address=arena.device.address+slot*sizeof(SignalABI)+offsetof(SignalABI,value);
        std::array<uint8_t,36> args{};
        std::memcpy(args.data(),&address,8);std::memcpy(args.data()+8,&result.device.address,8);
        std::memcpy(args.data()+16,&value,8);std::memcpy(args.data()+24,&compare,8);
        const uint32_t op=operation;std::memcpy(args.data()+32,&op,4);
        auto status=connection->writeBuffer(arguments,0,args.data(),args.size());
        if (status!=HSA_STATUS_SUCCESS) return fail();
        amdgpu::AQLDispatchRequest request{};
        request.version=1;request.codeHandle=code.handle;request.descriptorOffset=object.kernels[0].descriptor;
        request.kernargHandle=arguments.handle;request.kernargBytes=args.size();request.timeoutUS=100000;
        for (unsigned i=0;i<3;++i) request.groups[i]=request.threads[i]=1;
        request.threads[0]=32;request.buffers[0]=result.device.handle;request.buffers[1]=arena.device.handle;
        uint64_t completion=UINT64_MAX;
        status=connection->dispatchAQL(request,completion);
        // Every queue slot is held by this process's public queues (a single
        // slot is held by any one queue), so no GPU launch was attempted.
        if (status==HSA_STATUS_ERROR_OUT_OF_RESOURCES) return executeOnCPU(slot,operation,value,compare,old);
        if (status!=HSA_STATUS_SUCCESS || completion) return fail();
        if (service) {
            const auto *trace=std::getenv("MAC_HSA_SIGNAL_TRACE");
            if (trace && std::strcmp(trace,"1")==0)
                std::fprintf(stderr,"signal-mailbox: backend=one-shot fallback-completed\n");
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memcpy(&old,result.host,8);return true;
    }
};
std::mutex contextsMutex;
std::map<Connection *,std::weak_ptr<GPUSignalContext>> contexts;
struct SignalSlot {
    std::shared_ptr<GPUSignalContext> context;
    unsigned index;
    SignalSlot(std::shared_ptr<GPUSignalContext> c,unsigned i):context(std::move(c)),index(i) {}
    ~SignalSlot() {std::lock_guard lock(context->slotsMutex);context->signals[index].reset();context->used[index]=false;}
};
}
hsa_status_t reclaimGPUSignalService(const std::shared_ptr<Connection> &connection,std::shared_ptr<void> *lease) {
    std::shared_ptr<GPUSignalContext> context;
    {
        std::lock_guard lock(contextsMutex);
        const auto found=contexts.find(connection.get());
        if (found!=contexts.end()) context=found->second.lock();
    }
    if (!context || !context->service) return HSA_STATUS_SUCCESS;
    struct Lease {
        std::shared_ptr<GPUSignalContext> context;
        std::unique_lock<std::mutex> lock;
        explicit Lease(std::shared_ptr<GPUSignalContext> c):context(std::move(c)),lock(context->operationsMutex) {}
    };
    try {
        auto held=std::make_shared<Lease>(context);
        if (!context->service->reclaim()) {context->fail();return HSA_STATUS_ERROR;}
        if (lease) *lease=std::move(held);
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) {return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
}
void invalidateGPUSignals(const std::shared_ptr<Connection> &connection) {
    std::shared_ptr<GPUSignalContext> context;
    {
        std::lock_guard lock(contextsMutex);
        const auto found=contexts.find(connection.get());
        if (found!=contexts.end()) context=found->second.lock();
    }
    if (context) context->fail();
}
hsa_status_t createGPUSignalBacking(const std::shared_ptr<Connection> &connection,int64_t initial,const std::shared_ptr<Signal> &signal) {
    {
        // A device that reports no queue slots can run no GPU work at all
        // (no queue, no bounded launch): its signals are only ever touched
        // by CPU threads, so they stay host signals instead of failing.
        DeviceSnapshot snapshot;
        const auto status=connection->read(snapshot);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (snapshot.queueSlotsReported && !snapshot.queueSlots) return HSA_STATUS_SUCCESS;
    }
    std::shared_ptr<GPUSignalContext> context;
    {
        std::lock_guard lock(contextsMutex);
        std::erase_if(contexts,[](const auto &entry) {return entry.second.expired();});
        context=contexts[connection.get()].lock();
        if (!context) {
            context=std::make_shared<GPUSignalContext>(connection);
            const auto status=context->initialize();
            if (status!=HSA_STATUS_SUCCESS) return status;
            contexts[connection.get()]=context;
        }
    }
    std::unique_lock lock(context->slotsMutex);
    if (context->faulted) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    unsigned slot=0;while (slot<context->used.size() && context->used[slot]) ++slot;
    if (slot==context->used.size()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    context->used[slot]=true;lock.unlock();
    std::shared_ptr<SignalSlot> backing;
    try {backing=std::make_shared<SignalSlot>(context,slot);}
    catch (...) {std::lock_guard rollback(context->slotsMutex);context->used[slot]=false;throw;}
    auto *abi=static_cast<SignalABI *>(context->arena.host)+slot;
    *abi={};abi->kind=1;abi->value=initial;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    signal->gpuAtomic=[context,slot](unsigned op,int64_t value,int64_t compare,int64_t &old) {
        return context->execute(slot,op,value,compare,old);
    };
    signal->gpuHealthy=[context] {return !context->faulted && (!context->service || context->service->healthy());};
    signal->sharedStorage=std::move(backing);signal->sharedABI=abi;
    signal->gpuConnection=context->connection;
    {
        std::lock_guard publish(context->slotsMutex);
        if (context->faulted) {signal->alive=false;return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
        context->signals[slot]=signal;
    }
    return HSA_STATUS_SUCCESS;
}
}
