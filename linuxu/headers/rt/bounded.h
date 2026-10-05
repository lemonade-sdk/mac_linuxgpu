/* A call bounded in time for a caller that must not sleep long (the
 * driver's incoming-call thread): the work runs on a wait-pool thread
 * (rt/wait_pool.h), and the caller waits for it at most a bounded time.
 * Work that overruns keeps running; it then owns its argument and frees it
 * when it ends. Runtime: linuxu/src/amdgpu-rt/bounded.c. */
#ifndef LINUXU_RT_BOUNDED_H
#define LINUXU_RT_BOUNDED_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run @fn(@arg) on a pool thread and wait up to @ms for it. 0: it ran to
 * the end, the caller owns @arg (and frees it). -ETIMEDOUT: it is still
 * running, and @release(@arg) runs on its thread when it ends. -EAGAIN or
 * -ENOMEM: it did not start; the caller owns @arg. Linux errno values. */
int rt_bounded_run(void (*fn)(void *arg), void *arg, void (*release)(void *arg),
		   unsigned int ms);
/* Bounded calls whose work overran and still runs. */
unsigned int rt_bounded_overrunning(void);

#ifdef __cplusplus
}
#endif
#endif
