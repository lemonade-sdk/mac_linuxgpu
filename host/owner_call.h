/* A MacLinuxGPU selector called the way the driver runs every call that can
 * sleep (dext/sources/session_state.h, "Calls that never sleep, and every
 * other call"): with IOConnectCallAsyncMethod, the completion awaited on the
 * calling thread, and the structure output, if any, fetched with
 * OWNER_RESULT. To its caller it behaves as IOConnectCallMethod does: the
 * selector's IOReturn, its scalar outputs and its structure output.
 *
 * Header only (static inline), for the host app (through its bridging
 * header), the HSA runtime and libmlg_drm. Each call has a notification
 * port of its own (header-only code keeps no state: Swift imports these
 * functions, not static variables). */
#ifndef MLG_OWNER_CALL_H
#define MLG_OWNER_CALL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

/* session_state.h's numbers (scripts/test-owner-call.sh checks they match). */
#define MLG_OWNER_CALL_SELECTOR_PING	0u
#define MLG_OWNER_CALL_SELECTOR_RESULT	88u
#define MLG_OWNER_CALL_HEADER		4u
#define MLG_OWNER_CALL_SCALARS		12u
#define MLG_OWNER_CALL_WORDS		(MLG_OWNER_CALL_HEADER + MLG_OWNER_CALL_SCALARS)

struct mlg_owner_call_waiter {
	bool done;
	kern_return_t status;
	uint64_t args[MLG_OWNER_CALL_WORDS];
	uint32_t nargs;
};

static inline void mlg_owner_call_completed(void *refcon, IOReturn result, void **args,
					    uint32_t nargs)
{
	struct mlg_owner_call_waiter *w = (struct mlg_owner_call_waiter *)refcon;

	w->status = result;
	w->nargs = nargs < MLG_OWNER_CALL_WORDS ? nargs : MLG_OWNER_CALL_WORDS;
	for (uint32_t i = 0; i < w->nargs; ++i)
		w->args[i] = (uint64_t)(uintptr_t)args[i];
	w->done = true;
}

/* Wait for @w's completion on @port. The driver ends every call itself (a
 * session call's own bounds, the session's close); each second without
 * one, a Ping notices a driver that went away (kIOReturnNotAttached). */
static inline kern_return_t mlg_owner_call_wait(io_connect_t connection, IONotificationPortRef port,
						struct mlg_owner_call_waiter *w)
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
			uint64_t pong = 0;
			uint32_t n = 1;

			if (IOConnectCallScalarMethod(connection, MLG_OWNER_CALL_SELECTOR_PING, NULL, 0,
						      &pong, &n) != kIOReturnSuccess)
				return kIOReturnNotAttached;
			continue;
		}
		if (kr != MACH_MSG_SUCCESS)
			return kIOReturnIPCError;
		IODispatchCalloutFromMessage(NULL, &msg.header, port);
	}
	return kIOReturnSuccess;
}

/* @selector with @input and @input_struct, as IOConnectCallMethod would
 * call it: on return *@output_count scalars are in @output and
 * *@output_struct_size bytes in @output_struct (pass NULLs for none). The
 * result is the selector's IOReturn, or a transport failure (the call did
 * not start, or the driver went away). */
static inline kern_return_t mlg_owner_call_on(IONotificationPortRef port, io_connect_t connection,
					      uint32_t selector,
					      const uint64_t *input, uint32_t input_count,
					      const void *input_struct, size_t input_struct_size,
					      uint64_t *output, uint32_t *output_count,
					      void *output_struct, size_t *output_struct_size)
{
	struct mlg_owner_call_waiter w;
	io_user_reference_t ref[kIOAsyncCalloutCount];
	uint64_t immediate[MLG_OWNER_CALL_SCALARS];
	uint32_t want = output_count ? *output_count : 0, n;
	size_t struct_cap = output_struct_size ? *output_struct_size : 0;
	uint8_t struct_probe = 0;
	kern_return_t kr;

	if (want > MLG_OWNER_CALL_SCALARS)
		want = MLG_OWNER_CALL_SCALARS;
	memset(&w, 0, sizeof(w));
	memset(ref, 0, sizeof(ref));
	ref[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)(uintptr_t)mlg_owner_call_completed;
	ref[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)(uintptr_t)&w;
	/* The immediate reply carries only the token; the scalar and structure
	 * capacities tell the driver what the caller asked of the selector. */
	n = want;
	{
		size_t probe_size = struct_cap;

		kr = IOConnectCallAsyncMethod(connection, selector, IONotificationPortGetMachPort(port),
					      ref, kIOAsyncCalloutCount, input, input_count,
					      input_struct, input_struct_size, immediate, &n,
					      struct_cap ? output_struct : (void *)&struct_probe,
					      struct_cap ? &probe_size : NULL);
	}
	if (kr != kIOReturnSuccess)
		return kr;
	kr = mlg_owner_call_wait(connection, port, &w);
	if (kr != kIOReturnSuccess)
		return kr;
	if (w.status != kIOReturnSuccess)
		return w.status;
	if (w.nargs < MLG_OWNER_CALL_HEADER || w.args[2] > MLG_OWNER_CALL_SCALARS ||
	    w.nargs < MLG_OWNER_CALL_HEADER + w.args[2])
		return kIOReturnIPCError;
	if ((kern_return_t)w.args[1] != kIOReturnSuccess)
		return (kern_return_t)w.args[1];
	n = (uint32_t)w.args[2];
	if (output_count) {
		if (n > *output_count)
			n = *output_count;
		for (uint32_t i = 0; i < n; ++i)
			output[i] = w.args[MLG_OWNER_CALL_HEADER + i];
		*output_count = n;
	}
	if (w.args[3]) {
		/* The structure output, kept by the driver until fetched. */
		uint64_t token = w.args[0];
		size_t size = struct_cap;
		uint32_t none = 0;

		if (!output_struct || w.args[3] > struct_cap)
			return kIOReturnNoSpace;
		kr = IOConnectCallMethod(connection, MLG_OWNER_CALL_SELECTOR_RESULT, &token, 1, NULL, 0,
					 NULL, &none, output_struct, &size);
		if (kr != kIOReturnSuccess)
			return kr;
		*output_struct_size = size;
	} else if (output_struct_size) {
		*output_struct_size = 0;
	}
	return kIOReturnSuccess;
}

static inline kern_return_t mlg_owner_call(io_connect_t connection, uint32_t selector,
					   const uint64_t *input, uint32_t input_count,
					   const void *input_struct, size_t input_struct_size,
					   uint64_t *output, uint32_t *output_count,
					   void *output_struct, size_t *output_struct_size)
{
	IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
	kern_return_t kr;

	if (!port)
		return kIOReturnNoMemory;
	kr = mlg_owner_call_on(port, connection, selector, input, input_count, input_struct,
			       input_struct_size, output, output_count, output_struct,
			       output_struct_size);
	IONotificationPortDestroy(port);
	return kr;
}

#endif
