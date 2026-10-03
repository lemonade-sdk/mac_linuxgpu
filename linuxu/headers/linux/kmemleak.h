/* Linux leak-scanner trace hook when CONFIG_DEBUG_KMEMLEAK is disabled.
 * Allocation ownership and canary tracking remain in the heap implementation. */
#ifndef _LINUX_KMEMLEAK_H
#define _LINUX_KMEMLEAK_H

static inline void kmemleak_update_trace(const void *pointer)
{
	(void)pointer;
}

#endif
