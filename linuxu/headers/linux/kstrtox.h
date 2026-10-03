/* linuxu: SHIM (third_party/linux/include/linux/kstrtox.h)
 *
 * String-to-number conversion. Runtime: linuxu/src/shims/*.c if linked;
 * for host compilation the driver only needs the prototypes.
 */
#ifndef _LINUX_KSTRTOX_H
#define _LINUX_KSTRTOX_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/uaccess.h>
#include <linux/limits.h>

/* __user qualifier is a no-op in the host shim */
int __must_check _kstrtol(const char *s, unsigned int base, long *res);
int __must_check _kstrtoul(const char *s, unsigned int base,
			   unsigned long *res);
int __must_check kstrtoull(const char *s, unsigned int base,
			   unsigned long long *res);
int __must_check kstrtoll(const char *s, unsigned int base, long long *res);

static inline int __must_check kstrtol(const char *s, unsigned int base, long *res)
{
	if (base == 16)
		return kstrtoll(s, base, (long long *)res);
	return _kstrtol(s, base, res);
}

static inline int __must_check kstrtoul(const char *s, unsigned int base,
					unsigned long *res)
{
	if (base == 16)
		return kstrtoull(s, base, (unsigned long long *)res);
	return _kstrtoul(s, base, res);
}

static inline int __must_check kstrtoint(const char *s, unsigned int base,
					 int *res)
{
	long value;
	int error = kstrtol(s, base, &value);
	if (error) return error;
	if (value < INT_MIN || value > INT_MAX) return -ERANGE;
	if (!res) return -EINVAL;
	*res = (int)value;
	return 0;
}

static inline int __must_check kstrtouint(const char *s, unsigned int base,
					  unsigned int *res)
{
	unsigned long value;
	int error = kstrtoul(s, base, &value);
	if (error) return error;
	if (value > UINT_MAX) return -ERANGE;
	if (!res) return -EINVAL;
	*res = (unsigned int)value;
	return 0;
}

static inline int __must_check kstrtou64(const char *s, unsigned int base,
					 u64 *res)
{
	return kstrtoull(s, base, (unsigned long long *)res);
}

/* Linux process addresses require a process-aware copy backend. */
static inline int __must_check kstrtol_from_user(const char __user *s,
						 size_t count, unsigned int base,
						 long *res)
{
	(void)s; (void)count; (void)base; (void)res;
	return -EFAULT;
}

static inline int __must_check kstrtouint_from_user(const char __user *s,
						    size_t count, unsigned int base,
						    unsigned int *res)
{
	(void)s; (void)count; (void)base; (void)res;
	return -EFAULT;
}

static inline int __must_check kstrtoul_from_user(const char __user *s,
						  size_t count, unsigned int base,
						  unsigned long *res)
{
	(void)s; (void)count; (void)base; (void)res;
	return -EFAULT;
}

static inline int __must_check kstrtou32(const char *s, unsigned int base,
					 u32 *res)
{
	return kstrtouint(s, base, (unsigned int *)res);
}

#endif /* _LINUX_KSTRTOX_H */
