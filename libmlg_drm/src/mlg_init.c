/* libmlg_drm: bringing the GPU up from a Linux-file client (mlg_transport.h).
 *
 * InitDevice probes the upstream driver, and the probe maps the GART at a
 * host address the client chose beforehand (HostWindow, selector 54); with
 * none chosen, compute start fails (gart status -19). So a client first
 * asks for the window's size (54 with 0), reserves twice that much of its
 * own address space, chooses the size-aligned base inside it (54 with the
 * base), initializes, and releases the reservation, as the HSA runtime,
 * the host app and drm-selftest.py --init do. The window is only a
 * placement hint: the driver maps the GART itself during the probe.
 *
 * Without a placed window the GPU would come up unable to run compute, so
 * initialization stops there instead, with the reason on stderr. */
#include <stdbool.h>
#include <stdio.h>
#include <sys/mman.h>

#include "mlg_transport.h"
#include <rt/lx_abi.h>

#define MLG_LX_EIO		5
#define MLG_LX_EOPNOTSUPP	95

/* The window sizes the driver may answer: a power of two, at least a page. */
static bool window_size_ok(uint64_t bytes)
{
	return bytes >= 16384 && !(bytes & (bytes - 1)) && bytes <= (1ull << 45);
}

int mlg_init_device(mlg_scalar_fn scalar, void *ctx)
{
	uint64_t query[1] = { 0 }, window[3] = { 0 }, base[1];
	void *reservation;
	size_t reserved;
	uint64_t bytes;
	int r;

	r = scalar(ctx, MLG_SELECTOR_HOST_WINDOW, query, 1, window, 3);
	if (r == -MLG_LX_ENOTTY) {
		fprintf(stderr, "libmlg_drm: the installed MacLinuxGPU driver does not let "
			"Linux-file clients place the host window (HostWindow, selector 54), which "
			"the GPU's initialization needs; install driver build 232 or later\n");
		return -MLG_LX_EOPNOTSUPP;
	}
	if (r) {
		fprintf(stderr, "libmlg_drm: the host window query failed (Linux errno %d); "
			"the GPU was not initialized\n", -r);
		return r;
	}
	bytes = window[1];
	if (!window_size_ok(bytes)) {
		fprintf(stderr, "libmlg_drm: the driver answered a host window of %#llx bytes, "
			"which is not a usable size; the GPU was not initialized\n",
			(unsigned long long)bytes);
		return -MLG_LX_EIO;
	}
	reserved = (size_t)bytes * 2;
	reservation = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (reservation == MAP_FAILED) {
		fprintf(stderr, "libmlg_drm: cannot reserve %#llx bytes for the host window; "
			"the GPU was not initialized\n", (unsigned long long)reserved);
		return -MLG_LX_ENOMEM;
	}
	base[0] = ((uint64_t)(uintptr_t)reservation + bytes - 1) & ~(bytes - 1);
	r = scalar(ctx, MLG_SELECTOR_HOST_WINDOW, base, 1, window, 3);
	if (!r && (window[0] != base[0] || window[1] != bytes))
		r = -MLG_LX_EIO;
	if (r) {
		fprintf(stderr, "libmlg_drm: the driver did not take the host window at %#llx "
			"(Linux errno %d); the GPU was not initialized\n",
			(unsigned long long)base[0], -r);
	} else {
		r = scalar(ctx, MLG_SELECTOR_INIT_DEVICE, NULL, 0, NULL, 0);
	}
	munmap(reservation, reserved);
	return r;
}
