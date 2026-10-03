/* linuxu: SHIM (third_party/linux/include/kunit/test-bug.h), CONFIG_KUNIT=n. */
#ifndef _KUNIT_TEST_BUG_H
#define _KUNIT_TEST_BUG_H

#include <linux/stddef.h>

struct kunit;

static inline struct kunit *kunit_get_current_test(void) { return NULL; }

#define kunit_fail_current_test(fmt, ...) do {} while (0)

#endif /* _KUNIT_TEST_BUG_H */
