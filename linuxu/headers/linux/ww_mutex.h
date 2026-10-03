/* linuxu: SHIM (third_party/linux/include/linux/ww_mutex.h)
 *
 * Wait/wound mutex API surface. Used by TTM (ttm_bo.c) and drm core
 * (drm_exec) for reservation-object locking. Runtime:
 * linuxu/src/sync/ww_mutex.c.
 */
#ifndef _LINUX_WW_MUTEX_H
#define _LINUX_WW_MUTEX_H

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/stdarg.h>

struct ww_class {
	atomic_long_t stamp;
	const char *acquire_name;
	const char *mutex_name;
	unsigned int is_wait_die;
};

struct ww_mutex {
	struct mutex base;
	struct ww_acquire_ctx *ctx;
};

struct ww_acquire_ctx {
	struct task_struct *task;
	atomic_long_t stamp;
	unsigned int acquired;
	unsigned short wounded;
	unsigned short is_wait_die;
};

struct task_struct;

#define DEFINE_WW_CLASS(classname) \
	struct ww_class classname = { \
		.stamp = LONGATOMIC_INIT(0), \
		.acquire_name = #classname "_acquire", \
		.mutex_name = #classname "_mutex", \
	}

/* standard ww classes (dma-resv / ttm) */
extern struct ww_class reservation_ww_class;

static inline void ww_mutex_init(struct ww_mutex *lock,
				 struct ww_class *ww_class)
{
	mutex_init(&lock->base);
	lock->ctx = NULL;
}

static inline void ww_acquire_init(struct ww_acquire_ctx *ctx,
				   struct ww_class *ww_class)
{
	ctx->task = NULL;
	ctx->stamp.counter = atomic_long_inc_return(&ww_class->stamp);
	ctx->acquired = 0;
	ctx->wounded = 0;
	ctx->is_wait_die = ww_class->is_wait_die;
}

static inline void ww_acquire_done(struct ww_acquire_ctx *ctx)
{
	(void)ctx;
}

static inline void ww_acquire_fini(struct ww_acquire_ctx *ctx)
{
	/* no-op in shim */
}

/* ---- runtime (linuxu/src/sync/ww_mutex.c) ---- */
extern int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
extern int ww_mutex_lock_interruptible(struct ww_mutex *lock,
				       struct ww_acquire_ctx *ctx);
extern void ww_mutex_unlock(struct ww_mutex *lock);
extern int ww_mutex_trylock(struct ww_mutex *lock,
			    struct ww_acquire_ctx *ctx);

static inline void ww_mutex_lock_slow(struct ww_mutex *lock,
				      struct ww_acquire_ctx *ctx)
{
	/* The caller has dropped its other reservations before the slow retry. */
	(void)ww_mutex_lock(lock, ctx);
}

static inline int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
						   struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock_interruptible(lock, ctx);
}

static inline void ww_mutex_destroy(struct ww_mutex *lock)
{
	mutex_destroy(&lock->base);
}

static inline bool ww_mutex_is_locked(struct ww_mutex *lock)
{
	return mutex_is_locked(&lock->base);
}

#endif /* _LINUX_WW_MUTEX_H */
