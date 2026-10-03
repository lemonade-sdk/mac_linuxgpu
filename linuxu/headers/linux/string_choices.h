/* linuxu: SHIM (third_party/linux/include/linux/string_choices.h) */
#ifndef _LINUX_STRING_CHOICES_H
#define _LINUX_STRING_CHOICES_H

#include <stdbool.h>
#include <linux/kstrtox.h>

static inline const char *str_true_false(bool v)
{
	return v ? "true" : "false";
}
#define str_false_true(v)		str_true_false(!(v))

static inline const char *str_yes_no(bool v)
{
	return v ? "yes" : "no";
}
#define str_no_yes(v)		str_yes_no(!(v))

#ifndef str_enabled_disabled
static inline const char *str_enabled_disabled(bool v)
{
	return v ? "enabled" : "disabled";
}
#define str_disabled_enabled(v)		str_enabled_disabled(!(v))
#endif

#endif
