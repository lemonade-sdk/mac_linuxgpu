/* host/selector_call.h against mock drivers: a selector that must be async
 * goes async only to a driver that serves it so (RuntimeBuild's compiled
 * build >= MLG_SESSION_CALLS_ASYNC_BUILD); to an older one (build 242, or
 * one whose RuntimeBuild answers three words) it fails at once with
 * kIOReturnUnsupported, never waiting for a completion that will not come.
 * The protocol is asked once per connection; calls that never sleep are
 * synchronous everywhere. Only the IOKit calls are replaced. */
#include <assert.h>
#include <stdio.h>
#include <IOKit/IOKitLib.h>

static kern_return_t mock_scalar(mach_port_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
static kern_return_t mock_method(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, size_t,
				 uint64_t *, uint32_t *, void *, size_t *);
static kern_return_t mock_async(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const uint64_t *,
				uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
static IONotificationPortRef mock_port_create(mach_port_t);
static void mock_port_destroy(IONotificationPortRef);
static mach_port_t mock_port_machport(IONotificationPortRef);
#define IOConnectCallScalarMethod mock_scalar
#define IOConnectCallMethod mock_method
#define IOConnectCallAsyncMethod mock_async
#define IONotificationPortCreate mock_port_create
#define IONotificationPortDestroy mock_port_destroy
#define IONotificationPortGetMachPort mock_port_machport
#include "selector_call.h"

static uint64_t driver_build;		/* RuntimeBuild out[3]; 0: three words only */
static int build_fails, builds, syncs, asyncs, ports;

static kern_return_t mock_scalar(mach_port_t c, uint32_t selector, const uint64_t *in, uint32_t nin,
				 uint64_t *out, uint32_t *count)
{
	(void)c; (void)in; (void)nin;
	if (selector == MLG_SELECTOR_RUNTIME_BUILD) {
		++builds;
		if (build_fails)
			return kIOReturnNotAttached;
		out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = 0;
		if (driver_build && *count >= 4) {
			out[3] = driver_build;
			*count = 4;
		} else {
			*count = 3;
		}
		return kIOReturnSuccess;
	}
	++syncs;
	if (count) *count = 0;
	return kIOReturnSuccess;
}

static kern_return_t mock_method(mach_port_t c, uint32_t selector, const uint64_t *in, uint32_t nin,
				 const void *s, size_t sn, uint64_t *out, uint32_t *count, void *so, size_t *son)
{
	(void)s; (void)sn; (void)so; (void)son;
	return mock_scalar(c, selector, in, nin, out, count);
}

/* A 243 driver that refuses the call before starting it: nothing waits. */
static kern_return_t mock_async(mach_port_t c, uint32_t selector, mach_port_t wake, uint64_t *ref,
				uint32_t nref, const uint64_t *in, uint32_t nin, const void *s, size_t sn,
				uint64_t *out, uint32_t *count, void *so, size_t *son)
{
	(void)c; (void)selector; (void)wake; (void)ref; (void)nref; (void)in; (void)nin;
	(void)s; (void)sn; (void)out; (void)count; (void)so; (void)son;
	++asyncs;
	return kIOReturnBadArgument;
}

static IONotificationPortRef mock_port_create(mach_port_t main)
{
	(void)main;
	++ports;
	return (IONotificationPortRef)(uintptr_t)0x1000;
}
static void mock_port_destroy(IONotificationPortRef port) { (void)port; --ports; }
static mach_port_t mock_port_machport(IONotificationPortRef port) { (void)port; return 9; }

int main(void)
{
	const io_connect_t c = 7;
	uint64_t out[3] = {0};
	uint32_t n = 3;
	int state = 0;

	/* Calls that never sleep: synchronous, the protocol never asked. */
	assert(mlg_selector_call_on(c, &state, MLG_SELECTOR_PING, NULL, 0, NULL, 0, out, &n, NULL, NULL) ==
	       kIOReturnSuccess && syncs == 1 && builds == 0 && state == 0);

	/* A build-242 driver: InitDevice fails at once, no async call, and
	 * the protocol is asked once for the connection. */
	driver_build = 242;
	assert(mlg_selector_call_on(c, &state, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) ==
	       kIOReturnUnsupported && state < 0 && builds == 1 && asyncs == 0);
	assert(mlg_selector_call_on(c, &state, 16, out, 1, NULL, 0, out, &n, NULL, NULL) ==
	       kIOReturnUnsupported && builds == 1 && asyncs == 0);

	/* An older RuntimeBuild (three words): older still. */
	driver_build = 0;
	state = 0;
	assert(mlg_selector_call_on(c, &state, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) ==
	       kIOReturnUnsupported && state < 0 && asyncs == 0);

	/* No answer to RuntimeBuild: the driver is gone, nothing is assumed. */
	build_fails = 1;
	state = 0;
	assert(mlg_selector_call_on(c, &state, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) ==
	       kIOReturnNotAttached && state == 0 && asyncs == 0);
	build_fails = 0;

	/* A 243 driver: the call goes async (here refused before it starts,
	 * so the call returns that at once); the protocol is asked once. */
	driver_build = MLG_SESSION_CALLS_ASYNC_BUILD;
	state = 0;
	builds = 0;
	assert(mlg_selector_call_on(c, &state, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) ==
	       kIOReturnBadArgument && state > 0 && asyncs == 1 && builds == 1);
	assert(mlg_selector_call_on(c, &state, 16, out, 1, NULL, 0, out, &n, NULL, NULL) ==
	       kIOReturnBadArgument && asyncs == 2 && builds == 1 && ports == 0);
	/* The stateless form asks on every async call. */
	assert(mlg_selector_call(c, 9, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) ==
	       kIOReturnBadArgument && builds == 2);
	puts("PASS selector calls: async only to a driver that serves it so; an older driver's "
	     "session call fails at once, a missing one is reported gone, the protocol asked once "
	     "per connection, calls that never sleep stay synchronous");
	return 0;
}
