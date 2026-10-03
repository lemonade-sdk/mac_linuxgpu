/* linuxu: SHIM (third_party/linux/include/linux/lockdep.h)
 *
 * Lock debugging is off in the host build. The no-op assertion macros
 * (lockdep_assert_held etc.) are owned by <linux/spinlock.h>; this
 * header only provides the lockdep_map type used by vendored drm
 * headers (drm_client.h).
 */
#ifndef __LINUX_LOCKDEP_H
#define __LINUX_LOCKDEP_H

#include <linux/stddef.h>

struct lock_class_key;

/*
 * struct lockdep_map — the driver (drm_connector.c, sched_main.c) declares
 * static instances with a `.name` initializer, so the type must carry the
 * upstream field set (lockdep is otherwise inert on the host).
 */
struct lockdep_map {
	const char *name;
	unsigned int class;
	unsigned int recursion;
	struct lock_class_key *key;
	void *ret_ip;
};

#define STATIC_DEFINE(lockdep_map, name) lockdep_map name

#define lockdep_register_key(key) do { (void)(key); } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define lockdep_assert_none_held_preempt() do { } while (0)
#define lockdep_free_key_range(addr, size) do { (void)(addr); (void)(size); } while (0)

#ifndef _RET_IP_
#define _RET_IP_ __builtin_return_address(0)
#endif

/* With lockdep disabled, discard argument tokens as the upstream macros do.
 * This also permits callers whose diagnostic map is CONFIG_LOCKDEP-guarded. */
#define lock_acquire_shared_recursive(...) do { } while (0)
#define lock_release(...) do { } while (0)
#define lockdep_assert(c) do { (void)(c); } while (0)

#endif /* __LINUX_LOCKDEP_H */
