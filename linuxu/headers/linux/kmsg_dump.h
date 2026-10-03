/* linuxu: SHIM (third_party/linux/include/linux/kmsg_dump.h)
 *
 * kmsg dump subsystem — the host build never runs dumpers; the struct
 * and register/unregister surface are provided so vendored drm headers
 * (drm_plane.h) compile.
 */
#ifndef __LINUX_KMSG_DUMP_H
#define __LINUX_KMSG_DUMP_H

#include <linux/list.h>

enum kmsg_dump_on_panic {
	KMSG_DUMP_ON_PANIC,
	KMSG_DUMP_ON_OOPS,
	KMSG_DUMP_NOOP,
};

struct kmsg_dump_detail;

struct kmsg_dumper {
	struct list_head list;
	void (*dump)(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail);
	enum kmsg_dump_on_panic level;
};

static inline int kmsg_dump_register(struct kmsg_dumper *dumper)
{
	(void)dumper;
	return 0;
}

static inline int kmsg_dump_unregister(struct kmsg_dumper *dumper)
{
	(void)dumper;
	return 0;
}

#endif /* __LINUX_KMSG_DUMP_H */
