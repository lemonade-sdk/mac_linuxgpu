/* libmlg_drm internals: the default transport. */
#ifndef MLG_TRANSPORT_H
#define MLG_TRANSPORT_H

#include "mlg_drm.h"

/* Fill @out with the platform's transport to the driver: 0, or a negative
 * Linux errno when there is none. */
int mlg_default_transport(struct mlg_transport *out);

/* MacAMDGPU selector numbers the Linux-file client also takes. */
#define MLG_SELECTOR_INIT_DEVICE	9u
#define MLG_SELECTOR_HOST_WINDOW	54u

/* One scalar call of the driver's user client: 0 or a negative Linux
 * errno (-ENOTTY for a selector the client may not call). */
typedef int (*mlg_scalar_fn)(void *ctx, uint32_t selector, const uint64_t *in, uint32_t nin,
			     uint64_t *out, uint32_t nout);
/* Initialize the GPU as a session client does (mlg_init.c): place the host
 * window, then InitDevice. Returns InitDevice's result, or, with no
 * InitDevice call and the reason on stderr, -EOPNOTSUPP when the driver
 * does not admit HostWindow from this client, -EIO when it answers an
 * unusable window or does not take the base, -ENOMEM when the window
 * cannot be reserved, or the query's own error. */
int mlg_init_device(mlg_scalar_fn scalar, void *ctx);

#endif
