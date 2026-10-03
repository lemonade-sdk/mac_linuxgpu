/* linuxu: AS-IS (third_party/linux/include/linux/array_size.h) */
#ifndef _LINUX_ARRAY_SIZE_H
#define _LINUX_ARRAY_SIZE_H

#include <linux/compiler.h>

#define __must_be_array(a) \
	(0 * sizeof(struct { int __array_is_required : \
		1 - 2 * __builtin_types_compatible_p(__typeof__(a), \
						 __typeof__(&(a)[0])); }))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))
#define ARRAY_END(arr)  (&(arr)[ARRAY_SIZE(arr)])

#endif  /* _LINUX_ARRAY_SIZE_H */
