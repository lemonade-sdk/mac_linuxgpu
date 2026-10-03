/* Reservation locks serialize context publication and deadlock arbitration.
 * The global condition only protects bookkeeping; unrelated held reservations
 * remain independent. Contexts and their locks need no auxiliary allocation. */
#include <pthread.h>
#include <linux/ww_mutex.h>
#include <linux/errno.h>
#include <linux/wait.h>

static pthread_mutex_t ww_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ww_changed = PTHREAD_COND_INITIALIZER;

static int older(const struct ww_acquire_ctx *a,
                 const struct ww_acquire_ctx *b)
{
	return (long)((unsigned long)a->stamp.counter -
	              (unsigned long)b->stamp.counter) < 0;
}

static int ww_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx, bool interruptible)
{
	int ret = 0;
	pthread_mutex_lock(&ww_state);
	if (ctx && !ctx->acquired)
		__atomic_store_n(&ctx->wounded, 0, __ATOMIC_RELAXED);
	for (;;) {
		if (ctx && lock->ctx == ctx) {
			ret = -EALREADY;
			break;
		}
		if (mutex_trylock(&lock->base)) {
			lock->ctx = ctx;
			if (ctx)
				ctx->acquired++;
			break;
		}
		if (ctx && ctx->acquired) {
			if (ctx->is_wait_die) {
				if (lock->ctx && older(lock->ctx, ctx)) {
					ret = -EDEADLK;
					break;
				}
			} else if (__atomic_load_n(&ctx->wounded, __ATOMIC_RELAXED)) {
				ret = -EDEADLK;
				break;
			}
		}
		if (ctx && !ctx->is_wait_die && lock->ctx && older(ctx, lock->ctx)) {
			/* The wounded owner may be waiting on another reservation. */
			if (!__atomic_exchange_n(&lock->ctx->wounded, 1, __ATOMIC_RELAXED))
				pthread_cond_broadcast(&ww_changed);
		}
		if (interruptible && linuxu_wait_signal_pending(false)) {
			ret = -EINTR;
			break;
		}
		struct timespec tick = { .tv_nsec = 1000000 };
		pthread_cond_timedwait_relative_np(&ww_changed, &ww_state, &tick);
	}
	pthread_mutex_unlock(&ww_state);
	return ret;
}

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{ return ww_lock(lock, ctx, false); }

int ww_mutex_lock_interruptible(struct ww_mutex *lock,
                               struct ww_acquire_ctx *ctx)
{
	return ww_lock(lock, ctx, true);
}

void ww_mutex_unlock(struct ww_mutex *lock)
{
	pthread_mutex_lock(&ww_state);
	if (lock->ctx)
		lock->ctx->acquired--;
	lock->ctx = NULL;
	mutex_unlock(&lock->base);
	pthread_cond_broadcast(&ww_changed);
	pthread_mutex_unlock(&ww_state);
}

int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;
	pthread_mutex_lock(&ww_state);
	ret = mutex_trylock(&lock->base);
	if (ret) {
		lock->ctx = ctx;
		if (ctx) {
			if (!ctx->acquired)
				__atomic_store_n(&ctx->wounded, 0, __ATOMIC_RELAXED);
			ctx->acquired++;
		}
	}
	pthread_mutex_unlock(&ww_state);
	return ret;
}
