/* What the host's display call gave IOConnectCallAsyncMethod
 * (display_call_host.c), for test_display_async.cpp. */
#ifndef DISPLAY_CALL_CAPTURE_H
#define DISPLAY_CALL_CAPTURE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct display_call_capture {
	uint32_t selector, scalar_count, out_words;
	int has_completion, has_output;
	uint64_t scalars[3];
	size_t input_size, output_capacity;
	uint8_t input[512];
};
void display_call_host_capture(uint64_t op, uint64_t arg, const void *input, size_t input_size,
			       struct display_call_capture *out);
#ifdef __cplusplus
}
#endif
#endif
