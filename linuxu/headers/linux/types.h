/* linuxu: SHIM (third_party/linux/include/linux/types.h)
 *
 * FOUNDATION header. Contract for every shim header: pure libc-based
 * typedefs + small core structs. No kernel-only includes (libc only:
 * <stdint.h>, <stddef.h>, <stdbool.h>).
 */
#pragma push_macro("ffs")
#undef ffs
#include <string.h>
/* linuxu: fold in the kernel string header (memset32 etc.) for every TU */
#include <linux/string.h>
#pragma pop_macro("ffs")

#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#include <linux/autoconf.h>


#include <stdint.h>
#include <stddef.h>

/* stdbool included after to avoid the kernel's `typedef _Bool bool` clash
 * with glibc's bool macro. */
#include <stdbool.h>
#undef bool
#include <stdbool.h>
#define bool _Bool

/*
 * Kernel integer types. Linux defines these as plain (non-singed-prefixed)
 * typedefs of the same width; we follow.
 */
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

#ifdef __SIZEOF_INT128__
typedef __int128  __s128;
typedef unsigned __int128 __u128;
typedef __s128 s128;
typedef __u128 u128;
#endif

/*
 * Endian-qualified types.  On arm64 little-endian (the only target), these
 * are plain types. The names exist so driver code compiles unmodified.
 */
#define __bitwise
/*
 * __packed — used by struct definitions in the display/edid headers to
 * suppress padding.  The kernel defines this in compiler_attributes.h;
 * we provide it here so that those headers compile standalone.
 */
#ifndef __packed
#define __packed __attribute__((packed))
#endif

typedef uint8_t  __le8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint8_t  __be8;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint64_t __aligned_be64;
typedef uint64_t __aligned_le64;
typedef uint64_t __aligned_u64;
typedef int64_t  __aligned_s64;
typedef uint64_t aligned_u64;
typedef int64_t  aligned_s64;
typedef __aligned_be64 aligned_be64;
typedef __aligned_le64 aligned_le64;

#define __force

/*
 * Kernel scalar aliases (from uapi/linux/types.h in the kernel; we cannot
 * include uapi/ from a foundation header so these are re-defined here).
 */
typedef unsigned long   __kernel_ulong_t;
typedef unsigned int    __kernel_uid32_t;
typedef unsigned int    __kernel_gid32_t;
typedef unsigned short  __kernel_uid16_t;
typedef unsigned short  __kernel_gid16_t;
typedef unsigned int    __kernel_dev_t;
typedef unsigned long   __kernel_ino_t;
typedef unsigned short  __kernel_mode_t;
typedef long            __kernel_off_t;
typedef int             __kernel_pid_t;
typedef long            __kernel_daddr_t;
typedef int             __kernel_key_t;
typedef int             __kernel_suseconds_t;
typedef int             __kernel_timer_t;
typedef int             __kernel_clockid_t;
typedef int             __kernel_mqd_t;
/* __kernel_size_t: kernel builds use the 32-bit uapi convention
 * (unsigned int); on host the macOS SDK may have already typedef'd
 * it as size_t via <sys/types.h> before this header runs, so only
 * define it when absent.  The driver only ever reads these fields
 * as unsigned long through the shadow header path, so the width
 * difference does not affect the P0 gate. */
#if defined(__KERNEL__)
typedef unsigned long   __kernel_size_t;
#elif !defined(__KERNEL_SIZE_T_DEFINED)
typedef size_t          __kernel_size_t;
#define __KERNEL_SIZE_T_DEFINED 1
#endif
typedef long            __kernel_ssize_t;
typedef long            __kernel_ptrdiff_t;
typedef long            __kernel_clock_t;
typedef void *          __kernel_caddr_t;
typedef long				loff_t;
typedef unsigned long   __kernel_uoff_t;

/*
 * POSIX base types (dev_t/off_t/clockid_t/...): the macOS SDK defines
 * these via <sys/_types/*.h> — often pulled in transitively by libc
 * headers (stdlib.h, stdio.h, time.h) BEFORE this header is reached,
 * making a redefinition fatal.  The KMD only consumes the kernel
 * spelling via its own headers, and the values are ABI-compatible for
 * the paths the driver uses, so map the kernel typedefs onto the libc
 * ones when the SDK already defined them, and skip redefinition
 * otherwise.  umode_t (kernel-only) is always defined.
 */
#include <sys/types.h>
typedef unsigned short        umode_t;
/* Host and DriverKit clock calls share the SDK clockid_t ABI. */
#include <time.h>

/* bool provided by <stdbool.h> above */
typedef __kernel_uid32_t      uid_t;
typedef __kernel_gid32_t      gid_t;
typedef __kernel_uid16_t      uid16_t;
typedef __kernel_gid16_t      gid16_t;
typedef long                  intptr_t;

#ifndef _SIZE_T
#define _SIZE_T
typedef __kernel_size_t  size_t;
#endif
#ifndef _SSIZE_T
#define _SSIZE_T
typedef __kernel_ssize_t ssize_t;
#endif
#ifndef _PTRDIFF_T
#define _PTRDIFF_T
typedef __kernel_ptrdiff_t ptrdiff_t;
#endif
#ifndef _CLOCK_T
#define _CLOCK_T
typedef __kernel_clock_t clock_t;
#endif
#ifndef _CADDR_T
#define _CADDR_T
typedef __kernel_caddr_t caddr_t;
#endif

/* bsd / sysv aliases */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef unsigned char  unchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef unsigned long long ullong;

/* uapi __kernel_* time types (normally from uapi/linux/types.h) */
typedef long __kernel_time64_t;
typedef unsigned long __kernel_old_time64_t;
typedef long __kernel_old_suseconds_t;

typedef s64 ktime_t;          /* Nanosecond kernel time scalar */
typedef u64 sector_t;
/* blkcnt_t: the macOS SDK owns it (via sys/types.h) as a signed
 * long long; the driver never compares blkcnt_t against u64, so let
 * the SDK's definition stand. */

#define READ 0
#define WRITE 1

#define pgoff_t unsigned long

typedef u64 dma_addr_t;       /* 64-bit bus addresses (arm64) */
typedef u64 phys_addr_t;      /* CONFIG_PHYS_ADDR_T_64BIT semantics */
typedef phys_addr_t resource_size_t;

typedef unsigned int gfp_t;    /* plain u32 (bitwise markers stripped) */
typedef unsigned int slab_flags_t;
typedef unsigned int fmode_t;


struct phys_vec {
	phys_addr_t paddr;
	size_t      len;
};

/*
 * atomic_t (32-bit): the kernel's is a struct { int counter; }.
 * The 64-bit family lives in linux/atomic.h; keep a
 * minimal definition here only if not already present.
 */
#ifndef _LINUX_TYPES_ATOMIC_T_DEFINED
#define _LINUX_TYPES_ATOMIC_T_DEFINED
typedef struct {
	int counter;
} atomic_t;
#define ATOMIC_INIT(i) { (i) }
#endif

typedef struct {
	atomic_t refcnt;
} rcuref_t;
#define RCUREF_INIT(i) { .refcnt = ATOMIC_INIT((i) - 1) }

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct ustat {
	__kernel_daddr_t f_tfree;
	unsigned long    f_tinode;
	char            f_fname[6];
	char            f_fpack[6];
};

/**
 * struct callback_head - callback structure for use with RCU and task_work.
 */
struct callback_head {
	struct callback_head *next;
	void (*func)(struct callback_head *head);
	/* kfree_rcu retains its enclosing allocation until the grace period. */
	void *linuxu_free_pointer;
	/* call_srcu: the SRCU domain whose grace period precedes func. */
	void *linuxu_srcu;
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head

typedef void (*rcu_callback_t)(struct rcu_head *head);
typedef void (*call_rcu_func_t)(struct rcu_head *head, rcu_callback_t func);

typedef void (*swap_r_func_t)(void *a, void *b, int size, const void *priv);
typedef void (*swap_func_t)(void *a, void *b, int size);
typedef int  (*cmp_r_func_t)(const void *a, const void *b, const void *priv);
typedef int  (*cmp_func_t)(const void *a, const void *b);

struct task_struct;

/**
 * struct rcuwait - block/wake a single task in an rcu-safe manner.
 */
struct rcuwait {
	struct task_struct *task;
};

/* Forward typedefs for common kernel types */
#ifndef _LINUX_WAIT_H
struct wait_queue_head;
typedef struct wait_queue_head wait_queue_head_t;
#endif

#ifndef _LINUX_WORKQUEUE_H
struct work_struct;
struct workqueue_struct;
#endif

#ifndef asmlinkage
#define asmlinkage
#endif

#ifndef __force
#define __force
#endif

#ifndef BITS_TO_LONGS
#define BITS_TO_LONGS(nr)	DIV_ROUND_UP(nr, BITS_PER_TYPE(long))
#endif

#ifndef small_const_nbits
#define small_const_nbits(nbits) \
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)
#endif

#ifndef BITS_PER_LONG
#define BITS_PER_LONG		64
#endif

#ifndef BITMASK_1TO
#define BITMASK_1TO(nbits, start)	(((nbits) == BITS_PER_LONG) ? ~0UL : ((1UL << (nbits)) - 1) << (start))
#endif
#ifndef BITMASK_FROM_0
#define BITMASK_FROM_0(nbits) \
	(((nbits) == BITS_PER_LONG) ? ~0UL : ((1UL << (nbits)) - 1))
#endif
#ifndef BITS_TO_LONGS
#define BITS_TO_LONGS(nr)	DIV_ROUND_UP(nr, BITS_PER_TYPE(long))
#endif

#ifndef small_const_nbits
#define small_const_nbits(nbits) \
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)
#endif

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG	64
#endif

#endif /* _LINUX_TYPES_H */

/* ---- display (DAL) architecture tokens ---- */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BIGENDIAN_CPU
#else
#define LITTLEENDIAN_CPU
#endif
#define le32_to_cpup(p) (*(const u32 *)(p))
#define le64_to_cpup(p) (*(const u64 *)(p))
#define be32_to_cpup(p) (*(const u32 *)(p))
#ifndef FALSE
#define FALSE 0
#define TRUE 1

/* Wave-4 cleanup: the _LINUX_TYPES_EXTRA_H block that followed (extern
 * dev_err_probe / vmemdup_array_user / str_yes_no / lock_acquire / …) was
 * amdgpu-worker pollution and has been removed. Each symbol now lives in its
 * proper header: fd_* in <linux/fs.h>, memdup/vmemdup_array_user in
 * <linux/uaccess.h>, dev_err_probe in <linux/dev_printk.h>, str_yes_no in
 * <linux/string_choices.h>, FBINFO_STATE_RUNNING in <linux/fb.h>,
 * file_clone_open in <linux/file.h>. */

#ifndef KHZ2PICOS
#define KHZ2PICOS(khz) ((1000000000LL) / (khz) * 1000)
#endif

#endif
