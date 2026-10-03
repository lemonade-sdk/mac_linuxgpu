/* linuxu: SHIM (third_party/linux/include/linux/util_macros.h)
 *
 * Generic utility macros needed by the AS-IS vendored drm headers
 * (drm_util.h). Subset of the vendor header; values AS-IS.
 */
#ifndef _LINUX_HELPER_MACROS_H_
#define _LINUX_HELPER_MACROS_H_

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler.h>

#define for_each_if(condition) if (!(condition)) {} else

#define find_closest(x, a, as)						\
({									\
	typeof(as) __fc_i, __fc_as = (as) - 1;				\
	long __fc_mid_x, __fc_x = (x);					\
	long __fc_left, __fc_right;					\
	typeof(*a) const *__fc_a = (a);					\
	for (__fc_i = 0; __fc_i < __fc_as; __fc_i++) {			\
		__fc_mid_x = (__fc_a[__fc_i] + __fc_a[__fc_i + 1]) / 2;	\
		if (__fc_x <= __fc_mid_x) {				\
			__fc_left = __fc_x - __fc_a[__fc_i];		\
			__fc_right = __fc_a[__fc_i + 1] - __fc_x;	\
			if (__fc_right < __fc_left)			\
				__fc_i++;				\
			break;						\
		}							\
	}								\
	(__fc_i);							\
})

/**
 * for_each_set_bit_wrap - variant of for_each_set_bit() which starts
 * searching from @pos
 */
#ifndef for_each_set_bit_from
#define for_each_set_bit_from(pos, mask, size) \
	for ((pos) = (pos) + 1; (pos) < (size); (pos)++)
#endif

#endif /* _LINUX_HELPER_MACROS_H_ */
