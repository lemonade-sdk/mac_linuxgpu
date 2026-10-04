/* Threads that sleep for clients: the shared half of interrupt-driven
 * waits.
 *
 * A client's wait (a KFD signal event, a fence) must not block the queue
 * its call arrived on, so the call registers the wait and hands it here;
 * a pool thread runs it (it sleeps until the interrupt that ends it, or
 * its timeout) and then completes the client's asynchronous call. Pool
 * threads are created on demand, up to RT_WAIT_POOL_THREADS, and an idle
 * one blocks on a condition with no timeout: no request means no thread
 * runs. Runtime: linuxu/src/amdgpu-rt/wait_pool.c. */
#ifndef LINUXU_RT_WAIT_POOL_H
#define LINUXU_RT_WAIT_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#define RT_WAIT_POOL_THREADS	32u

/* Run @fn(@arg) on a pool thread. -EAGAIN when every thread is busy (the
 * caller's wait did not start), -ENOMEM when no thread could be made. */
int rt_wait_pool_run(void (*fn)(void *arg), void *arg);

struct rt_wait_pool_stats {
	unsigned int threads;	/* created */
	unsigned int busy;	/* running a wait now */
	unsigned long long runs;	/* waits run */
	unsigned long long refused;	/* -EAGAIN */
};
void rt_wait_pool_stats(struct rt_wait_pool_stats *out);

#ifdef __cplusplus
}
#endif
#endif
