/* linuxu: SHIM (third_party/linux/include/linux/minmax.h)
 *
 * 3- and 4-argument min/max forms. Reuses the 2-arg min()/max() from
 * <linux/bitops.h>; "careful" pairwise combine as in vendor minmax.h.
 */
#ifndef __LINUX_MAX_H
#define __LINUX_MAX_H

#define __careful_min(a, b) \
	({ \
		typeof(a) __a = (a); \
		typeof(b) __b = (b); \
		(void) (&__a == &__b); \
		__a <= __b ? __a : __b; \
	})

#define __careful_max(a, b) \
	({ \
		typeof(a) __a = (a); \
		typeof(b) __b = (b); \
		(void) (&__a == &__b); \
		__a >= __b ? __a : __b; \
	})

#define min3(x, y, z) \
	min(__careful_min(x, y), z)

#define max3(x, y, z) \
	max(__careful_max(x, y), z)

#define min4(x, y, z, w) \
	min(min3(x, y, z), w)

#define max4(x, y, z, w) \
	max(max3(x, y, z), w)

#endif /* __LINUX_MAX_H */
