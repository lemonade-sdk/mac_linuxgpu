/* Park and unpark: how linuxu threads sleep without polling.
 *
 * A futex over addresses. linuxu_park(key, still, arg, deadline) sleeps
 * while still(arg) holds, until linuxu_unpark(key) or the deadline
 * (CLOCK_UPTIME_RAW ns, 0 for none). A waker changes the state still()
 * reads, then unparks the key: still() is evaluated under the key's
 * bucket lock, so a wake between the waiter's check and its sleep is
 * never lost. A sleeper wakes only for its key's unpark (or another key's
 * in the same bucket, then sleeps again) and its deadline.
 *
 * Tasks park on themselves: linuxu_task_wake() (wake_up_process, wake
 * queues, signals, kthread_stop) advances task->wake_sequence and unparks
 * the task, and linuxu_park_task() sleeps until it moves. wait_event*,
 * schedule_timeout, completions, contended mutexes and dma_fence waits
 * are built on this (linux/wait.h, linuxu/src/sync.c, dma_fence.c).
 *
 * The backstop. Linux code is written for wakes, but a linuxu shim (or a
 * test fixture) that changes a condition without waking would otherwise
 * hang its waiter. A wait whose condition it can test sleeps at most the
 * backstop, first linuxu_park_backstop_first() and doubling up to
 * linuxu_park_backstop_max() while nothing happens, so an idle waiter
 * costs a wakeup per backstop_max and nothing more. A backstop that finds
 * the condition already true is a missing wake: it is logged once per
 * wait site (linuxu_wait_missed) and counted. Runtime:
 * linuxu/src/shims/task.c. */
#ifndef LINUXU_RT_PARK_H
#define LINUXU_RT_PARK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task_struct;

#define LINUXU_PARK_TIMEOUT	0
#define LINUXU_PARK_WOKEN	1

uint64_t linuxu_park_now_ns(void);
int linuxu_park(const void *key, bool (*still)(const void *arg), const void *arg,
		uint64_t deadline_ns);
void linuxu_unpark(const void *key);
/* Until @task's wake_sequence moves from @seq, or the deadline. */
int linuxu_park_task(struct task_struct *task, unsigned long seq, uint64_t deadline_ns);

#define LINUXU_PARK_BACKSTOP_FIRST_NS	2000000ULL	/* 2 ms */
#define LINUXU_PARK_BACKSTOP_MAX_NS	1000000000ULL	/* 1 s */
void linuxu_park_set_backstop(uint64_t first_ns, uint64_t max_ns);
uint64_t linuxu_park_backstop_first(void);
uint64_t linuxu_park_backstop_max(void);
static inline uint64_t linuxu_park_backstop_next(uint64_t current_ns)
{
	const uint64_t max = linuxu_park_backstop_max();

	return current_ns >= max / 2 ? max : current_ns * 2;
}

/* A wait site (a wait_event in source), for the once-per-site log. */
struct linuxu_wait_site {
	const char *file;
	int line;
	int logged;
};
/* A backstop found the wait's condition true: no wake came. @site, or
 * when NULL the caller's return address @caller, is logged once. */
void linuxu_wait_missed(struct linuxu_wait_site *site, const void *caller);
/* Writes one report line (weak: stderr, or printk where printk.c is). */
void linuxu_wait_report(const char *message);

struct linuxu_park_stats {
	unsigned long long parks;	/* sleeps begun */
	unsigned long long woken;	/* ended by a state change */
	unsigned long long timeouts;	/* ended by a deadline or backstop */
	unsigned long long wakeups;	/* times a sleeper's thread ran again */
	unsigned long long missed;	/* backstops that found the condition true */
};
void linuxu_park_stats(struct linuxu_park_stats *out);

#ifdef __cplusplus
}
#endif
#endif
