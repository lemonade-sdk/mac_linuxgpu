/* libmlg_drm: bringing the GPU up from a Linux-file client (mlg_transport.h).
 *
 * InitDevice probes the upstream driver, and the probe maps the GART at a
 * host address the client chose beforehand (HostWindow, selector 54); with
 * none chosen, compute start fails (gart status -19). So a client first
 * asks for the window's size (54 with 0), reserves twice that much of its
 * own address space, chooses the size-aligned base inside it (54 with the
 * base), initializes, and releases the reservation, as the HSA runtime,
 * the host app and drm-selftest.py --init do. The window is only a
 * placement hint: the driver maps the GART itself during the probe. */
#include <stdbool.h>
#include <sys/mman.h>

#include "mlg_transport.h"
#include <rt/lx_abi.h>

/* The window sizes the driver may answer: a power of two, at least a page. */
static bool window_size_ok(uint64_t bytes)
{
	return bytes >= 16384 && !(bytes & (bytes - 1)) && bytes <= (1ull << 45);
}

int mlg_init_device(mlg_scalar_fn scalar, void *ctx)
{
	uint64_t query[1] = { 0 }, window[3] = { 0 };
	void *reservation = NULL;
	size_t reserved = 0;
	int r;

	r = scalar(ctx, MLG_SELECTOR_HOST_WINDOW, query, 1, window, 3);
	if (!r && window_size_ok(window[1])) {
		const uint64_t bytes = window[1];

		reserved = (size_t)bytes * 2;
		reservation = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (reservation == MAP_FAILED) {
			reservation = NULL;
		} else {
			uint64_t base[1] = { ((uint64_t)(uintptr_t)reservation + bytes - 1) &
					     ~(bytes - 1) };

			/* A driver that does not take the base still probes:
			 * InitDevice reports what that costs. */
			(void)scalar(ctx, MLG_SELECTOR_HOST_WINDOW, base, 1, window, 3);
		}
	}
	/* -ENOTTY: a driver that does not admit HostWindow from a Linux-file
	 * client (0.1.127 and earlier); initialize as before. */
	r = scalar(ctx, MLG_SELECTOR_INIT_DEVICE, NULL, 0, NULL, 0);
	if (reservation)
		munmap(reservation, reserved);
	return r;
}
