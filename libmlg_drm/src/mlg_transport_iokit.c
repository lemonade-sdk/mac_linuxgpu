/* The IOKit transport of libmlg_drm: a Linux-file user client (type
 * MLG_USER_CLIENT_LINUX_FILE) of the MacLinuxGPU DriverKit extension, and
 * the selectors of rt/lx_abi.h. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#include "mlg_transport.h"
#include <rt/lx_abi.h>

/* MacAMDGPU selector numbers the Linux-file client also takes (and
 * mlg_transport.h's). */
#define MLG_SELECTOR_PING	0u

struct iokit {
	pthread_mutex_t lock;
	io_connect_t conn;
};
static struct iokit io = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int from_ioreturn(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess: return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoResources: return -MLG_LX_ENOMEM;
	case kIOReturnBadArgument: return -MLG_LX_EINVAL;
	case kIOReturnNotPermitted:
	case kIOReturnNotPrivileged: return -1;	/* EPERM */
	case kIOReturnNoSpace: return -MLG_LX_ENOSPC;
	case kIOReturnBusy: return -MLG_LX_EBUSY;
	case kIOReturnUnsupported: return -MLG_LX_ENOTTY;
	case kIOReturnAborted: return -MLG_LX_EINTR;
	case kIOReturnNotReady:
	case kIOReturnNotOpen:
	case kIOReturnNotAttached:
	case kIOReturnNoDevice: return -MLG_LX_ENODEV;
	default: return -5;	/* EIO */
	}
}

static bool is_driver(io_service_t service)
{
	CFTypeRef cls = IORegistryEntryCreateCFProperty(service, CFSTR("IOUserClass"),
							kCFAllocatorDefault, 0);
	bool match = cls && CFGetTypeID(cls) == CFStringGetTypeID() &&
		     CFStringCompare((CFStringRef)cls, CFSTR("MacLinuxGPU"), 0) == kCFCompareEqualTo;

	if (cls)
		CFRelease(cls);
	return match;
}

/* The first MacLinuxGPU service, or the one MLG_DRM_REGISTRY_ID names. */
static int connect_locked(void)
{
	const char *want = getenv("MLG_DRM_REGISTRY_ID");
	uint64_t wanted = want ? strtoull(want, NULL, 0) : 0;
	io_iterator_t it = IO_OBJECT_NULL;
	io_service_t service;
	kern_return_t kr = kIOReturnNoDevice;

	if (io.conn)
		return 0;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUserService"),
					 &it) != KERN_SUCCESS)
		return -MLG_LX_ENODEV;
	while ((service = IOIteratorNext(it))) {
		uint64_t id = 0;

		if (is_driver(service) && (!wanted ||
		    (IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS && id == wanted)))
			kr = IOServiceOpen(service, mach_task_self(), MLG_USER_CLIENT_LINUX_FILE,
					   &io.conn);
		IOObjectRelease(service);
		if (io.conn)
			break;
	}
	IOObjectRelease(it);
	return io.conn ? 0 : from_ioreturn(kr == kIOReturnSuccess ? kIOReturnNoDevice : kr);
}

static int conn(io_connect_t *out)
{
	int r;

	pthread_mutex_lock(&io.lock);
	r = connect_locked();
	*out = io.conn;
	pthread_mutex_unlock(&io.lock);
	return r;
}

static int scalar(uint32_t selector, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
	io_connect_t c;
	uint32_t n = nout;
	int r = conn(&c);

	if (r)
		return r;
	return from_ioreturn(IOConnectCallScalarMethod(c, selector, in, nin, out, out ? &n : NULL));
}

static int scalar_call(void *ctx, uint32_t selector, const uint64_t *in, uint32_t nin,
		       uint64_t *out, uint32_t nout)
{
	(void)ctx;
	return scalar(selector, in, nin, out, nout);
}

static int t_open(void *ctx, uint32_t dev, uint32_t flags)
{
	uint64_t in[2] = { dev, flags }, out[1] = { 0 };
	int r;

	(void)ctx;
	r = scalar(MLG_SELECTOR_LX_OPEN, in, 2, out, 1);
	if (r == -MLG_LX_ENODEV) {
		/* The driver is attached but the GPU is not initialized yet:
		 * initialize it, as a session client does (host window, then
		 * InitDevice), then retry; the open fails with the reason the
		 * initialization did. */
		r = mlg_init_device(scalar_call, NULL);
		if (!r)
			r = scalar(MLG_SELECTOR_LX_OPEN, in, 2, out, 1);
	}
	return r ? r : (int)(int64_t)out[0];
}

static int t_close(void *ctx, int fd)
{
	uint64_t in[1] = { (uint64_t)(int64_t)fd }, out[1] = { 0 };
	int r;

	(void)ctx;
	r = scalar(MLG_SELECTOR_LX_CLOSE, in, 1, out, 1);
	return r ? r : (int)(int64_t)out[0];
}

/* ---- async completions ---- */

struct waiter {
	bool done;
	kern_return_t status;
	uint64_t args[MLG_LX_ASYNC_WORDS];
	uint32_t nargs;
};

static void completed(void *refcon, IOReturn result, void **args, uint32_t nargs)
{
	struct waiter *w = refcon;

	w->status = result;
	w->nargs = nargs < MLG_LX_ASYNC_WORDS ? nargs : MLG_LX_ASYNC_WORDS;
	for (uint32_t i = 0; i < w->nargs; ++i)
		w->args[i] = (uint64_t)(uintptr_t)args[i];
	w->done = true;
}

/* One notification port per thread: a thread has one wait in flight. */
static pthread_key_t port_key;
static pthread_once_t port_once = PTHREAD_ONCE_INIT;

static void port_free(void *p)
{
	IONotificationPortDestroy(p);
}

static void port_init(void)
{
	pthread_key_create(&port_key, port_free);
}

static IONotificationPortRef thread_port(void)
{
	IONotificationPortRef port;

	pthread_once(&port_once, port_init);
	port = pthread_getspecific(port_key);
	if (!port) {
		port = IONotificationPortCreate(kIOMainPortDefault);
		if (port)
			pthread_setspecific(port_key, port);
	}
	return port;
}

static int wait_completion(io_connect_t c, IONotificationPortRef port, struct waiter *w)
{
	union {
		mach_msg_header_t header;
		uint8_t bytes[4096];
	} msg;

	while (!w->done) {
		kern_return_t kr;

		memset(&msg.header, 0, sizeof(msg.header));
		kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg),
			      IONotificationPortGetMachPort(port), 1000, MACH_PORT_NULL);
		if (kr == MACH_RCV_TIMED_OUT) {
			/* The wait has its own deadline in the driver; this only
			 * notices a driver that went away. */
			uint64_t pong = 0;
			uint32_t n = 1;

			if (IOConnectCallScalarMethod(c, MLG_SELECTOR_PING, NULL, 0, &pong, &n) !=
			    kIOReturnSuccess)
				return -5;	/* EIO */
			continue;
		}
		if (kr != MACH_MSG_SUCCESS)
			return -5;
		IODispatchCalloutFromMessage(NULL, &msg.header, port);
	}
	return 0;
}

static int t_ioctl(void *ctx, int fd, uint32_t cmd, const void *frame, size_t frame_bytes,
		   void *reply, size_t reply_cap, size_t *reply_bytes, int64_t *result, int async)
{
	uint64_t in[2] = { (uint64_t)(int64_t)fd, cmd }, out[2] = { 0, 0 };
	uint32_t n = 2;
	size_t size = reply_cap;
	io_connect_t c;
	int r = conn(&c);

	(void)ctx;
	if (r)
		return r;
	if (!async) {
		r = from_ioreturn(IOConnectCallMethod(c, MLG_SELECTOR_LX_IOCTL, in, 2, frame, frame_bytes,
						      out, &n, reply, &size));
		if (r)
			return r;
		*result = (int64_t)out[0];
		*reply_bytes = (size_t)out[1];
		return *reply_bytes <= reply_cap ? 0 : -MLG_LX_EINVAL;
	}

	IONotificationPortRef port = thread_port();
	struct waiter w = { 0 };
	io_user_reference_t ref[kIOAsyncCalloutCount] = { 0 };

	if (!port)
		return -MLG_LX_ENOMEM;
	ref[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)(uintptr_t)completed;
	ref[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)(uintptr_t)&w;
	r = from_ioreturn(IOConnectCallAsyncMethod(c, MLG_SELECTOR_LX_IOCTL_ASYNC,
						   IONotificationPortGetMachPort(port), ref,
						   kIOAsyncCalloutCount, in, 2, frame, frame_bytes,
						   out, &n, NULL, NULL));
	if (!r && (int64_t)out[0] < 0)
		r = (int)(int64_t)out[0];	/* not started */
	if (r)
		return r;
	r = wait_completion(c, port, &w);
	if (r)
		return r;
	if (w.status != kIOReturnSuccess)
		return w.status == kIOReturnAborted ? -MLG_LX_EINTR : from_ioreturn(w.status);
	if (w.nargs < 3 || w.args[0] != out[1] || w.args[2] > reply_cap)
		return -5;
	*result = (int64_t)w.args[1];
	*reply_bytes = (size_t)w.args[2];
	if (*reply_bytes <= MLG_LX_ASYNC_INLINE_BYTES && w.nargs >= 3 + (*reply_bytes + 7) / 8) {
		memcpy(reply, &w.args[3], *reply_bytes);
		return 0;
	}
	/* Too long for the completion: fetch it. */
	uint64_t token[1] = { out[1] };

	n = 2;
	size = reply_cap;
	r = from_ioreturn(IOConnectCallMethod(c, MLG_SELECTOR_LX_RESULT, token, 1, NULL, 0, out, &n,
					      reply, &size));
	if (r)
		return r;
	*result = (int64_t)out[0];
	*reply_bytes = (size_t)out[1];
	return 0;
}

/* ---- mmap ---- */

static int t_mmap(void *ctx, int fd, uint64_t offset, uint64_t length, uint32_t prot,
		  uint32_t flags, void **addr, uint64_t *handle)
{
	uint64_t in[5] = { (uint64_t)(int64_t)fd, offset, length, prot, flags }, out[4] = { 0 };
	mach_vm_address_t at = 0;
	mach_vm_size_t size = 0;
	IOOptionBits options = kIOMapAnywhere;
	io_connect_t c;
	int r = conn(&c);

	(void)ctx;
	if (r)
		return r;
	r = scalar(MLG_SELECTOR_LX_MMAP, in, 5, out, 4);
	if (!r)
		r = (int)(int64_t)out[0];
	if (r)
		return r;
	if (out[3] == 1)
		options |= kIOMapWriteCombineCache;
	else if (out[3] == 2)
		options |= kIOMapInhibitCache;
	else
		options |= kIOMapDefaultCache;
	r = from_ioreturn(IOConnectMapMemory64(c, (uint32_t)out[1], mach_task_self(), &at, &size,
					       options));
	if (r || size < length) {
		uint64_t type[1] = { out[1] }, ignored[1];

		if (!r)
			IOConnectUnmapMemory64(c, (uint32_t)out[1], mach_task_self(), at);
		(void)scalar(MLG_SELECTOR_LX_MUNMAP, type, 1, ignored, 1);
		return r ? r : -MLG_LX_EINVAL;
	}
	/* Place it at the same address in the driver's process too (best
	 * effort: the mapping works either way). */
	{
		uint64_t commit[2] = { out[1], at }, ignored[1];

		(void)scalar(MLG_SELECTOR_LX_MMAP_COMMIT, commit, 2, ignored, 1);
	}
	*addr = (void *)(uintptr_t)at;
	*handle = out[1];
	return 0;
}

static int t_munmap(void *ctx, uint64_t handle, void *addr, uint64_t length)
{
	uint64_t type[1] = { handle }, out[1] = { 0 };
	io_connect_t c;
	int r = conn(&c);

	(void)ctx;
	(void)length;
	if (r)
		return r;
	r = from_ioreturn(IOConnectUnmapMemory64(c, (uint32_t)handle, mach_task_self(),
						 (mach_vm_address_t)(uintptr_t)addr));
	if (r)
		return r;
	r = scalar(MLG_SELECTOR_LX_MUNMAP, type, 1, out, 1);
	return r ? r : (int)(int64_t)out[0];
}

/* ---- identity: the IOPCIDevice the driver matched ---- */

static bool data_property(io_registry_entry_t entry, CFStringRef key, uint32_t *value)
{
	CFTypeRef p = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
	bool ok = false;

	if (p && CFGetTypeID(p) == CFDataGetTypeID() && CFDataGetLength(p) >= 4) {
		const uint8_t *b = CFDataGetBytePtr(p);

		*value = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
			 (uint32_t)b[3] << 24;
		ok = true;
	}
	if (p)
		CFRelease(p);
	return ok;
}

static int t_identity(void *ctx, struct mlg_pci_identity *out)
{
	const char *want = getenv("MLG_DRM_REGISTRY_ID");
	uint64_t wanted = want ? strtoull(want, NULL, 0) : 0;
	io_iterator_t it = IO_OBJECT_NULL;
	io_service_t service;
	int r = -MLG_LX_ENODEV;

	(void)ctx;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUserService"),
					 &it) != KERN_SUCCESS)
		return -MLG_LX_ENODEV;
	while (r && (service = IOIteratorNext(it))) {
		io_registry_entry_t entry = service, parent;
		uint64_t id = 0;
		uint32_t v;

		if (!is_driver(service) || (wanted &&
		    (IORegistryEntryGetRegistryEntryID(service, &id) != KERN_SUCCESS ||
		     id != wanted))) {
			IOObjectRelease(service);
			continue;
		}
		IOObjectRetain(entry);
		/* The provider chain up to the PCI function. */
		while (!data_property(entry, CFSTR("vendor-id"), &v)) {
			kern_return_t kr = IORegistryEntryGetParentEntry(entry, kIOServicePlane,
									 &parent);

			IOObjectRelease(entry);
			if (kr != KERN_SUCCESS) {
				entry = IO_OBJECT_NULL;
				break;
			}
			entry = parent;
		}
		if (entry) {
			out->vendor_id = (uint16_t)v;
			if (data_property(entry, CFSTR("device-id"), &v))
				out->device_id = (uint16_t)v;
			if (data_property(entry, CFSTR("subsystem-vendor-id"), &v))
				out->subvendor_id = (uint16_t)v;
			if (data_property(entry, CFSTR("subsystem-id"), &v))
				out->subdevice_id = (uint16_t)v;
			if (data_property(entry, CFSTR("revision-id"), &v))
				out->revision_id = (uint8_t)v;
			/* Open Firmware "reg": the config address, bus 23:16,
			 * device 15:11, function 10:8. */
			if (data_property(entry, CFSTR("reg"), &v)) {
				out->bus = (uint8_t)(v >> 16);
				out->dev = (uint8_t)((v >> 11) & 0x1f);
				out->func = (uint8_t)((v >> 8) & 0x7);
			}
			IOObjectRelease(entry);
			r = 0;
		}
		IOObjectRelease(service);
	}
	IOObjectRelease(it);
	return r;
}

/* ---- LX_SCANOUT ---- */

static int t_scanout(void *ctx, const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state,
		     int64_t *result)
{
	uint64_t out[1] = { 0 };
	uint32_t nout = 1;
	size_t bytes = sizeof(*state);
	io_connect_t c;
	int r = conn(&c);

	(void)ctx;
	if (r)
		return r;
	r = from_ioreturn(IOConnectCallMethod(c, MLG_SELECTOR_LX_SCANOUT, NULL, 0, req, sizeof(*req),
					      out, &nout, state, &bytes));
	if (r)
		return r;
	if (nout < 1 || bytes != sizeof(*state))
		return -MLG_LX_EINVAL;
	*result = (int64_t)out[0];
	return 0;
}

int mlg_default_transport(struct mlg_transport *out)
{
	*out = (struct mlg_transport){
		.open = t_open, .close = t_close, .ioctl = t_ioctl,
		.mmap = t_mmap, .munmap = t_munmap, .identity = t_identity,
		.scanout = t_scanout,
	};
	return 0;
}
