/* linuxu: EDITED (third_party/linux/include/linux/log2.h)
 * ilog2() family is provided as static inlines in linux/kernel.h;
 * this header adds order_base_2() and ilog2_up() on top of that. */
#ifndef _LINUX_LOG2_H
#define _LINUX_LOG2_H

#include <linux/types.h>

#ifndef ilog2_up
#define ilog2_up(n)	((n) <= 1 ? 0 : ilog2((n) - 1) + 1)
#endif

#ifndef order_base_2
#define order_base_2(n) \
	((n) <= 1 ? 0 : ilog2((n) - 1) + 1)
#endif

static inline int __ilog2_u32(unsigned int v)
{
	int r = 0;
	while (v > 1) { v >>= 1; r++; }
	return r;
}


#endif /* _LINUX_LOG2_H */