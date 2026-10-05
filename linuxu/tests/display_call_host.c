/* The host app's display call (host/display_call.h), with IOKit's
 * IOConnectCallAsyncMethod replaced by a capture of what it was given:
 * test_display_async.cpp hands that to the driver's own display_call as
 * the kernel would, so the host's arguments meet the real validation. */
#include <string.h>

#include <IOKit/IOKitLib.h>

#include "display_call_capture.h"

static struct display_call_capture *capture_to;

static kern_return_t capture_async(mach_port_t connection, uint32_t selector, mach_port_t wake,
				   uint64_t *reference, uint32_t reference_count, const uint64_t *input,
				   uint32_t input_count, const void *input_struct, size_t input_struct_size,
				   uint64_t *output, uint32_t *output_count, void *output_struct,
				   size_t *output_struct_size)
{
	struct display_call_capture *c = capture_to;

	(void)connection;
	(void)output;
	memset(c, 0, sizeof(*c));
	c->selector = selector;
	c->has_completion = wake != MACH_PORT_NULL && reference && reference_count >= kIOAsyncCalloutCount;
	c->scalar_count = input_count < 3 ? input_count : 3;
	memcpy(c->scalars, input, c->scalar_count * sizeof(uint64_t));
	if (input_struct_size > sizeof(c->input))
		return kIOReturnNoSpace;
	c->input_size = input_struct_size;
	if (input_struct_size)
		memcpy(c->input, input_struct, input_struct_size);
	c->out_words = output_count ? *output_count : 0;
	c->has_output = output_struct != NULL && output_struct_size != NULL;
	c->output_capacity = c->has_output ? *output_struct_size : 0;
	return kIOReturnSuccess;
}

#define IOConnectCallAsyncMethod capture_async
#include "display_call.h"

void display_call_host_capture(uint64_t op, uint64_t arg, const void *input, size_t input_size,
			       struct display_call_capture *out)
{
	uint64_t reference[kIOAsyncCalloutCount] = { 0 };
	const uint64_t scalars[3] = { op, arg, MLG_DISPLAY_CONFIRM };
	uint64_t words[MLG_DISPLAY_WORDS];
	uint32_t count = MLG_DISPLAY_WORDS;

	capture_to = out;
	(void)mlg_display_call_start(1, 2, reference, kIOAsyncCalloutCount, scalars, input, input_size,
				     words, &count);
}
