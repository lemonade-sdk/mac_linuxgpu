/* libmlg_drm internals: the default transport. */
#ifndef MLG_TRANSPORT_H
#define MLG_TRANSPORT_H

#include "mlg_drm.h"

/* Fill @out with the platform's transport to the driver: 0, or a negative
 * Linux errno when there is none. */
int mlg_default_transport(struct mlg_transport *out);

#endif
