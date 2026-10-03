/* linuxu: EDITED (third_party/linux/include/linux/limits.h; vdso/uapi inlined) */
#ifndef _LINUX_LIMITS_H
#define _LINUX_LIMITS_H

#include <linux/types.h>
#include <limits.h>

#define NR_OPEN		1024
#define NGROUPS_MAX	65536
#define ARG_MAX		131072
#define LINK_MAX	127
#define MAX_CANON	255
#define MAX_INPUT	255
#define NAME_MAX	255
#define PATH_MAX	4096
#define PIPE_BUF	4096
#define XATTR_NAME_MAX	255
#define XATTR_SIZE_MAX	65536
#define XATTR_LIST_MAX	65536
#define RTSIG_MAX	32

#define USHRT_MAX	((unsigned short)~0U)
#define SHRT_MAX	((short)(USHRT_MAX >> 1))
#define SHRT_MIN	((short)(-SHRT_MAX - 1))
#define LONG_MAX	((long)(~0UL >> 1))
#define LONG_MIN	(-LONG_MAX - 1)
#define ULONG_MAX	(~0UL)
#define LLONG_MAX	((long long)(~0ULL >> 1))
#define LLONG_MIN	(-LLONG_MAX - 1)
#define ULLONG_MAX	(~0ULL)
#define UINTPTR_MAX	ULONG_MAX

#define SIZE_MAX	(~(size_t)0)
#define SSIZE_MAX	((ssize_t)(SIZE_MAX >> 1))
#define PHYS_ADDR_MAX	(~(phys_addr_t)0)
#define RESOURCE_SIZE_MAX	((resource_size_t)~0)

#define U8_MAX		((u8)~0U)
#define S8_MAX		((s8)(U8_MAX >> 1))
#define S8_MIN		((s8)(-S8_MAX - 1))
#define U16_MAX		((u16)~0U)
#define S16_MAX		((s16)(U16_MAX >> 1))
#define S16_MIN		((s16)(-S16_MAX - 1))
#define U32_MAX		((u32)~0U)
#define S32_MAX		((s32)(U32_MAX >> 1))
#define S32_MIN		((s32)(-S32_MAX - 1))
#define U64_MAX		((u64)~0ULL)
#define S64_MAX		((s64)(U64_MAX >> 1))
#define S64_MIN		((s64)(-S64_MAX - 1))

#endif /* _LINUX_LIMITS_H */
