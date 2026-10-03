/* linuxu: SHIM (third_party/linux/include/linux/stackdepot.h)
 *
 * Userspace has no stack depot: the handle type is carried so structs that
 * embed it (drm_mm_node, drm_modeset_lock) keep their layout; depot calls
 * are no-ops returning handle 0.
 */
#ifndef _LINUX_STACKDEPOT_H
#define _LINUX_STACKDEPOT_H

#include <linux/types.h>

typedef u32 depot_stack_handle_t;

#define DEPOT_HANDLE_INVALID 0

static inline depot_stack_handle_t
__stack_depot_save(void *addr, int skip, int rel_size, gfp_t gfp)
{
	return 0;
}

#define stack_depot_save(ip, size, gfp)		__stack_depot_save(ip, 0, 0, gfp)

static inline const char *stack_depot_fetch(depot_stack_handle_t handle,
					    char *out, size_t out_len)
{
	if (out_len)
		out[0] = '\0';
	return out;
}

static inline bool stack_depot_save_lookup(u32 *ip, int skip,
					   int rel_size, gfp_t gfp,
					   struct va_format *vaf)
{
	return false;
}

#endif
