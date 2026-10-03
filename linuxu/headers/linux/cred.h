/* Diagnostic credentials only. A DriverKit client is not a Linux UID. */
#ifndef __LINUXU_CRED_H
#define __LINUXU_CRED_H

#include <linux/types.h>

struct user_namespace;

struct cred {
	u32 euid;
};

/* Preserve Linux's root constant for diagnostic formatting. This shim never
 * uses debugfs output as an authentication or authorization decision. A
 * present shim task without credentials has unknown identity and renders as
 * the conventional overflow UID. */
#define LINUXU_UNKNOWN_KUID ((u32)~0U)
#define LINUXU_OVERFLOW_UID 65534U
#define GLOBAL_ROOT_UID ((u32)0)

static inline u32 from_kuid_munged(struct user_namespace *ns, u32 uid)
{
	(void)ns;
	return uid == LINUXU_UNKNOWN_KUID ? LINUXU_OVERFLOW_UID : uid;
}

#endif
