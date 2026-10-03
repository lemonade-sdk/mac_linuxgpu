/* linuxu: SHIM (third_party/linux/include/linux/compiler_attributes.h)
 * Clang-compatible attribute definitions.
 * __error__ is a GCC-specific construct; clang uses __builtin_unreachable().
 */
#ifndef __LINUX_COMPILER_ATTRIBUTES_H
#define __LINUX_COMPILER_ATTRIBUTES_H

#ifndef __always_inline
#define __always_inline		inline __attribute__((always_inline))
#endif
#ifndef __noinline
#define __noinline			__attribute__((noinline))
#endif
#ifndef __weak
#define __weak				__attribute__((weak))
#endif
#ifndef __cold
#define __cold				__attribute__((cold))
#endif
#ifndef __hot
#define __hot				__attribute__((hot))
#endif
#ifndef __must_check
#define __must_check			__attribute__((warn_unused_result))
#endif
#ifndef __printf
#define __printf(a, b)			__attribute__((format(printf, a, b)))
#endif
#ifndef __nonstring
#define __nonstring
#endif
#ifndef __alloc_size
#define __alloc_size(a, b)		__attribute__((alloc_size(a, b)))
#endif
#ifndef __packed
#define __packed				__attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(a)			__attribute__((aligned(a)))
#endif
#ifndef __visible
#define __visible				__attribute__((visibility("default")))
#endif
#ifndef __destructor
#define __destructor			__attribute__((destructor))
#endif
#ifndef __constructor
#define __constructor			__attribute__((constructor))
#endif
#ifndef __assume_aligned
#define __assume_aligned(b, a)		__builtin_assume_aligned((b), (a))
#endif
#ifndef __assume
#define __assume(x)				__builtin_assume(x)
#endif
#ifndef __compiletime_error
#define __compiletime_error(msg)		__attribute__((error(msg)))
#endif
#endif /* __LINUX_COMPILER_ATTRIBUTES_H */
