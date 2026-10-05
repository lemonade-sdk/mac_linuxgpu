/* The start of a display op that can sleep (dext/sources/session_state.h,
 * "Display ops"): an async call of MLG_SELECTOR_DISPLAY that the driver
 * answers at once with a token, then completes; the op's structure output
 * is fetched with RESULT. The call carries a structure output of
 * MLG_DISPLAY_REPORT_MAX bytes, every op's largest: the driver checks the
 * call's output capacity before it starts PROBE, SHOW, OFF, STATUS and
 * MODES and refuses a call without one (kIOReturnBadArgument: build 242's
 * host passed none, and no monitor was ever found).
 *
 * Header only, for the host app (bridging header) and its tests
 * (linuxu/tests/display_call_host.c runs it against the driver's own
 * validation). */
#ifndef MLG_DISPLAY_CALL_H
#define MLG_DISPLAY_CALL_H

#include <stdint.h>

#include <IOKit/IOKitLib.h>

#include "../dext/sources/session_state.h"

/* @scalars: [op, argument, MLG_DISPLAY_CONFIRM]. On success @out[1] is the
 * op's token (*@out_count MLG_DISPLAY_WORDS). */
static inline kern_return_t mlg_display_call_start(io_connect_t connection, mach_port_t wake,
						   uint64_t *reference, uint32_t reference_count,
						   const uint64_t scalars[3], const void *input,
						   size_t input_size, uint64_t out[MLG_DISPLAY_WORDS],
						   uint32_t *out_count)
{
	uint8_t reply[MLG_DISPLAY_REPORT_MAX];
	size_t reply_size = sizeof(reply);

	return IOConnectCallAsyncMethod(connection, MLG_SELECTOR_DISPLAY, wake, reference,
					reference_count, scalars, 3, input, input_size, out, out_count,
					reply, &reply_size);
}

#endif
