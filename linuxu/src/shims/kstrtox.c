/* Boolean parsing from lib/kstrtox.c (pinned upstream), including the
 * user-buffer form debugfs write handlers use. */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>

int kstrtobool(const char *s, bool *res)
{
	if (!s)
		return -EINVAL;

	switch (s[0]) {
	case 'e':
	case 'E':
	case 'y':
	case 'Y':
	case 't':
	case 'T':
	case '1':
		*res = true;
		return 0;
	case 'd':
	case 'D':
	case 'n':
	case 'N':
	case 'f':
	case 'F':
	case '0':
		*res = false;
		return 0;
	case 'o':
	case 'O':
		switch (s[1]) {
		case 'n':
		case 'N':
			*res = true;
			return 0;
		case 'f':
		case 'F':
			*res = false;
			return 0;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EINVAL;
}

/* Upstream kstrtobool_from_user(): at most 3 bytes are examined. */
int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[4] = { 0 };

	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count))
		return -EFAULT;

	return kstrtobool(buf, res);
}
