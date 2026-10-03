//
//  MacLinuxGPU.mm — Driver + UserClient implementation (make build path).
//
//  This is the compile-clean, link-clean implementation for the plain
//  `make dext` build. The real DriverKit class registration (OSMetaClass
//  sMetaClass + OSClassDescription method tables that make IOKit
//  instantiate MacLinuxGPU / MacLinuxGPUUserClient) is produced by the
//  Xcode DriverKit build-phase `iig` codegen from MacLinuxGPU.iig /
//  MacLinuxGPUUserClient.iig — that codegen is NOT run by a plain make
//  build, so it is stubbed here with a clear TODO.
//
//  This file compiles the attach-only lifecycle for the make path. The
//  activatable Xcode build provides the registered IOService and UserClient
//  methods, including the explicit InitDevice path for upstream PCI probe.
//
//  The plain make build has no UserClient registration and cannot initiate
//  GPU bringup.
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOUserClient.h>
#include <PCIDriverKit/IOPCIDevice.h>

// The C API seam holds the matched provider without opening its PCI session.
extern "C" int dext_set_pci(void *pci_device, void *client);
extern "C" void dext_close(void);
extern "C" int fw_table_register_embedded(void);

#define MACLINUXGPU_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.linuxgpu: " fmt, ##__VA_ARGS__)

// The retained provider and queue are released by the plain make lifecycle.
static IODispatchQueue *s_bringupQueue = nullptr;
static IOPCIDevice     *s_retainedPCI  = nullptr;
static bool             s_seamAttached = false;

// ----------------------------------------------------------------
// Driver (IOService) lifecycle for the unregistered make build. The
// registered Xcode class has its own method bodies.
// ----------------------------------------------------------------

// MacLinuxGPU::Start body. Retain the matched PCI provider and register it
// with the seam. A UserClient would open PCI and initiate probe explicitly.
static kern_return_t
mac_linuxgpu_driver_start(IOService *provider)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == nullptr) {
        MACLINUXGPU_LOG("provider is not an IOPCIDevice");
        return kIOReturnUnsupported;
    }

    int fw_result = fw_table_register_embedded();
    if (fw_result != 0) {
        MACLINUXGPU_LOG("firmware registration failed: %d", fw_result);
        return kIOReturnNoMemory;
    }

    // Keep the queue lifecycle consistent with the registered Xcode class.
    IODispatchQueue *bqueue = nullptr;
    kern_return_t qret = IODispatchQueue::Create("MacLinuxGPUBringup",
                                                 0, 0, &bqueue);
    if (qret != kIOReturnSuccess || bqueue == nullptr) {
        MACLINUXGPU_LOG("IODispatchQueue::Create (MacLinuxGPUBringup) "
                        "failed: %#x", qret);
        return qret != kIOReturnSuccess ? qret : kIOReturnNoMemory;
    }
    s_bringupQueue = bqueue;

    // Retain the PCI device for the driver's lifetime.
    pci->retain();
    s_retainedPCI = pci;

    // Hand the provider to the seam without opening PCI or starting IRQs.
    if (dext_set_pci(pci, provider) != 0) {
        MACLINUXGPU_LOG("dext_set_pci failed");
        s_retainedPCI->release();
        s_retainedPCI = nullptr;
        s_bringupQueue->release();
        s_bringupQueue = nullptr;
        return kIOReturnError;
    }
    s_seamAttached = true;

    // Log PCI identity and BAR sizes without mapping or initializing them.
    uint8_t bus = 0, device = 0, function = 0;
    pci->GetBusDeviceFunction(&bus, &device, &function);
    MACLINUXGPU_LOG("matched PCI provider %02x:%02x.%u",
                    (unsigned)bus, (unsigned)device, (unsigned)function);
    for (uint8_t bar = 0; bar < 6; bar++) {
        uint8_t  mi = 0;
        uint64_t sz = 0;
        uint8_t  ty = 0;
        if (pci->GetBARInfo(bar, &mi, &sz, &ty) == kIOReturnSuccess) {
            MACLINUXGPU_LOG("BAR%u memoryIndex=%u size=%llu type=%u",
                            (unsigned)bar, (unsigned)mi,
                            (unsigned long long)sz, (unsigned)ty);
        }
    }

    MACLINUXGPU_LOG("driver Start done (PCI provider attached; no GPU init)");
    return kIOReturnSuccess;
}

// MacLinuxGPU::Stop body. No PCI session, IRQ source, or GPU device was
// started by this unregistered make path.
static kern_return_t
mac_linuxgpu_driver_stop(IOService *provider)
{
    (void)provider;
    if (s_seamAttached) {
        dext_close();
        s_seamAttached = false;
    }
    return kIOReturnSuccess;
}

// The plain make build cannot create a registered UserClient.
static kern_return_t
mac_linuxgpu_driver_new_user_client(uint32_t type,
                                    IOUserClient **userClient)
{
    if (type != 0) {
        MACLINUXGPU_LOG("unsupported user-client type %u", (unsigned)type);
        return kIOReturnUnsupported;
    }
    // The Xcode-codegen'd path creates the MacLinuxGPUUserClient service
    // via Create(this, "MacLinuxGPUUserClientProperties") and returns the
    // typed IOUserClient. In the make build the UserClient class is not
    // OSMetaClass-registered (see the TODO below), so we return
    // kIOReturnUnsupported — the make dext is a compile+link milestone, not
    // an activatable driver. T-hostapp-sysextd (Xcode build) wires this.
    if (userClient)
        *userClient = nullptr;
    MACLINUXGPU_LOG("NewUserClient: UserClient registration is Xcode-"
                    "codegen (T-hostapp-sysextd); returning Unsupported "
                    "in the make build");
    return kIOReturnUnsupported;
}

// MacLinuxGPU::free body. Release retained attach resources.
static void
mac_linuxgpu_driver_free(void)
{
    if (s_seamAttached) {
        dext_close();
        s_seamAttached = false;
    }
    if (s_bringupQueue != nullptr) {
        s_bringupQueue->release();
        s_bringupQueue = nullptr;
    }
    if (s_retainedPCI != nullptr) {
        s_retainedPCI->release();
        s_retainedPCI = nullptr;
    }
    MACLINUXGPU_LOG("driver free: attach resources released");
}

// ----------------------------------------------------------------
// The OSMetaClass registration.
//
// TODO(T-hostapp-sysextd / Xcode DriverKit build): the real
//   OSMetaClass *MacLinuxGPU::sMetaClass,
//   OSMetaClass *MacLinuxGPUUserClient::sMetaClass,
//   const OSClassDescription ... (the method tables)
// are produced by the `iig` codegen from MacLinuxGPU.iig /
// MacLinuxGPUUserClient.iig and linked by the Xcode build. They make
// IOKit's class loader instantiate MacLinuxGPU on PCI match and spawn
// the UserClient per open. A plain make/clang build cannot run the
// codegen, so the make dext is a compile+link milestone (C API seam +
// KMD static lib) and is not activatable. The registered Xcode class
// supplies its own lifecycle methods.
//
// We DO NOT define a hand-rolled OSMetaClass here: the OSClassDescription
// method-table layout must match exactly what IOKit's class loader
// expects, and a wrong table is worse than none. The Xcode codegen is
// the single source of truth.
// ----------------------------------------------------------------
//
// The object is deliberately unregistered and dead-code-stripped in a plain
// make build. The registered Xcode class owns the active driver lifecycle.
// ----------------------------------------------------------------
