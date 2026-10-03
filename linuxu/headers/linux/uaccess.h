/* linuxu: SHIM (third_party/linux/include/linux/uaccess.h)
 *
 * User addresses belong to the linuxu process whose mm is current->mm.
 * The backend (linuxu/src/mm/uaccess.c) resolves each address through that
 * mm's VMA registry to the kernel-side backing the VMA carries (a BO's
 * kernel mapping, a userptr descriptor mapping, a per-call arena slot).
 * As on Linux, a task without an mm, an unmapped address, or a VMA without
 * the needed permission or backing faults: copies report the bytes left,
 * get_user/put_user return -EFAULT. access_ok only checks the range against
 * the user address-space limit, like Linux.
 * memdup/vmemdup_array_user mirror the vendor 2026 static inlines in
 * <linux/string.h> (ERR_PTR on overflow, backed by the linuxu/src
 * memdup_user/vmemdup_user definitions).
 */
#ifndef __LINUX_UACCESS_H
#define __LINUX_UACCESS_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/overflow.h>

/* user-to-kernel duplicate helpers (defined in linuxu/src/kmem/kmemalloc.c) */
extern void *memdup_user(const void __user *src, size_t size);
extern void *vmemdup_user(const void __user *src, size_t size);
/* linuxu: memdup_user_nul returns void* in the 2026 kernel (the driver
 * stores the result in a char* and kfree()s it); matches the driver
 * call sites in drm_ioctl.c. */
extern void *memdup_user_nul(const void __user *src, size_t size);

#define __user

#ifndef fallthrough
#define fallthrough do { } while (0)
#endif

static inline void *vmemdup_array_user(const void __user *src, size_t n, size_t size)
{
	size_t nbytes;

	if (check_mul_overflow(n, size, &nbytes))
		return ERR_PTR(-EOVERFLOW);
	return vmemdup_user(src, nbytes);
}

static inline void *memdup_array_user(const void __user *src, size_t n, size_t size)
{
	size_t nbytes;

	if (check_mul_overflow(n, size, &nbytes))
		return ERR_PTR(-EOVERFLOW);
	return memdup_user(src, nbytes);
}

/* arm64 user address-space limit (VA_BITS = 48), as in asm/processor.h. */
#ifndef TASK_SIZE_MAX
#define TASK_SIZE_MAX	(1UL << 48)
#endif

static inline int __access_ok(const void __user *addr, unsigned long size)
{
	unsigned long a = (unsigned long)addr;

	return size <= TASK_SIZE_MAX && a <= TASK_SIZE_MAX - size;
}
#define access_ok(addr, size) __access_ok((const void __user *)(addr), (size))

/* Each returns the number of bytes NOT transferred (0 on success). */
extern unsigned long linuxu_copy_to_user(void __user *to, const void *from,
					 unsigned long n);
extern unsigned long linuxu_copy_from_user(void *to, const void __user *from,
					   unsigned long n);
extern unsigned long linuxu_clear_user(void __user *to, unsigned long n);
extern long strncpy_from_user(char *dst, const char __user *src, long count);
extern long strnlen_user(const char __user *src, long n);

static inline unsigned long copy_to_user(void __user *to, const void *from,
					 unsigned long n)
{
	if (!n)
		return 0;
	if (!access_ok(to, n))
		return n;
	return linuxu_copy_to_user(to, from, n);
}

static inline unsigned long copy_from_user(void *to, const void __user *from,
					   unsigned long n)
{
	unsigned long left = n;

	if (!n)
		return 0;
	if (access_ok(from, n))
		left = linuxu_copy_from_user(to, from, n);
	/* Linux zeroes the destination bytes that could not be copied. */
	if (left)
		__builtin_memset((char *)to + (n - left), 0, left);
	return left;
}

static inline unsigned long clear_user(void __user *addr, unsigned long size)
{
	if (!size)
		return 0;
	if (!access_ok(addr, size))
		return size;
	return linuxu_clear_user(addr, size);
}
#define __copy_to_user(to, from, n)	copy_to_user((to), (from), (n))
#define __copy_from_user(to, from, n)	copy_from_user((to), (from), (n))
#define __clear_user(addr, n)		clear_user((addr), (n))

/* Scalars move through an aligned byte buffer so const-qualified and
 * pointer pointee types work. On a fault get_user stores zero. */
#define get_user(x, ptr) ({ \
	unsigned char __gu_buf[sizeof(*(ptr))] __attribute__((aligned(8))); \
	int __gu_err = copy_from_user(__gu_buf, (ptr), sizeof(*(ptr))) ? \
		-EFAULT : 0; \
	(x) = *(__typeof__(*(ptr)) *)(void *)__gu_buf; \
	__gu_err; \
})
#define put_user(x, ptr) ({ \
	__typeof__(*(ptr)) __pu_val = (x); \
	copy_to_user((ptr), &__pu_val, sizeof(__pu_val)) ? -EFAULT : 0; \
})
#define __get_user(x, ptr) get_user(x, ptr)
#define __put_user(x, ptr) put_user(x, ptr)

#endif /* __LINUX_UACCESS_H */
