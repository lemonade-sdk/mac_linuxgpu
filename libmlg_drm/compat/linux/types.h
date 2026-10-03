/* The Linux uapi integer types, for building the Linux uapi headers
 * (third_party/linux/include/uapi) in a macOS process. Only what the DRM,
 * amdgpu and KFD uapi headers use. */
#ifndef MLG_COMPAT_LINUX_TYPES_H
#define MLG_COMPAT_LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;
typedef uint16_t __le16;
typedef uint16_t __be16;
typedef uint32_t __le32;
typedef uint32_t __be32;
typedef uint64_t __le64;
typedef uint64_t __be64;
typedef size_t __kernel_size_t;
typedef long __kernel_ssize_t;
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define __aligned_s64 __s64 __attribute__((aligned(8)))

#ifndef __user
#define __user
#endif
#ifndef __bitwise
#define __bitwise
#endif

#endif
