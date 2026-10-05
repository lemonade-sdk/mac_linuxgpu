/* A MacLinuxGPU selector called as the driver serves it
 * (dext/sources/session_state.h): synchronously when it never sleeps (or is
 * a bounded read), else as an async session call awaited on this thread
 * (owner_call.h). Either way the caller sees what IOConnectCallMethod
 * returns: the selector's IOReturn, its scalars and its structure output.
 * Header only, for the host app (bridging header), the HSA runtime and
 * libmlg_drm. */
#ifndef MLG_SELECTOR_CALL_H
#define MLG_SELECTOR_CALL_H

#include "owner_call.h"
#include "../dext/sources/session_state.h"

static inline kern_return_t mlg_selector_call(io_connect_t connection, uint32_t selector,
					      const uint64_t *input, uint32_t input_count,
					      const void *input_struct, size_t input_struct_size,
					      uint64_t *output, uint32_t *output_count,
					      void *output_struct, size_t *output_struct_size)
{
#ifndef MLG_SELECTOR_CALL_TEST_SYNC
	if (mlg_call_is_synchronous(selector, input, input_count))
#endif
	{
		/* (Tests that replace the IOKit calls define
		 * MLG_SELECTOR_CALL_TEST_SYNC to see every selector here.) */
		if (!input_struct && !output_struct)
			return IOConnectCallScalarMethod(connection, selector, input, input_count,
							 output, output_count);
		return IOConnectCallMethod(connection, selector, input, input_count, input_struct,
					   input_struct_size, output, output_count, output_struct,
					   output_struct_size);
	}
	return mlg_owner_call(connection, selector, input, input_count, input_struct,
			      input_struct_size, output, output_count, output_struct,
			      output_struct_size);
}

#endif
