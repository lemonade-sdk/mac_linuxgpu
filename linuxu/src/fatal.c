/* linuxu shim: fatal — record, contain, then park (see rt/fatal.h).
 *
 * Only allocation-free, lock-free work happens before the thread parks:
 * one bounded snprintf into a stack buffer, a retained-ring append, the
 * platform log sink and the platform containment hook. Clock reads are
 * avoided because a failing clock is one of the callers. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <rt/fatal.h>
#include <rt/klog.h>

/* sync.c: approximate process-wide count of held spinlocks. */
extern long linuxu_spinlocks_held(void);

/* Threads currently reporting. A fatal raised while the same thread is
 * reporting (for example from the errno or stdio adapters the reporting
 * path uses, or from the hook) parks at once instead of recursing. */
#define LINUXU_FATAL_REPORTERS 8

static linuxu_fatal_hook_t fatal_hook;
static unsigned int fatal_entries;
static unsigned int fatal_parked_threads;
static uintptr_t fatal_reporters[LINUXU_FATAL_REPORTERS];

/* Returns the claimed slot, or -1 for recursion or no free slot. */
static int fatal_claim_reporter(uintptr_t self)
{
	for (int i = 0; i < LINUXU_FATAL_REPORTERS; ++i)
		if (__atomic_load_n(&fatal_reporters[i], __ATOMIC_ACQUIRE) == self)
			return -1;
	for (int i = 0; i < LINUXU_FATAL_REPORTERS; ++i) {
		uintptr_t expected = 0;
		if (__atomic_compare_exchange_n(&fatal_reporters[i], &expected,
				self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return i;
	}
	return -1;
}

void linuxu_fatal_set_hook(linuxu_fatal_hook_t hook)
{
	__atomic_store_n(&fatal_hook, hook, __ATOMIC_RELEASE);
}

unsigned int linuxu_fatal_count(void)
{
	return __atomic_load_n(&fatal_entries, __ATOMIC_ACQUIRE);
}

unsigned int linuxu_fatal_parked(void)
{
	return __atomic_load_n(&fatal_parked_threads, __ATOMIC_ACQUIRE);
}

static void fatal_report(unsigned int nth, const char *why, const char *file,
			 int line)
{
	char record[384];
	long spins = linuxu_spinlocks_held();
	int length = snprintf(record, sizeof(record),
		"<0> linuxu FATAL #%u: %s at %s:%d; thread %p parked forever, "
		"device quarantined%s\n",
		nth, why ? why : "?", file ? file : "?", line,
		(void *)pthread_self(),
		spins > 0 ? "; WARNING: spinlocks still held process-wide" : "");
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(record))
		length = (int)sizeof(record) - 1;
	klog_write(record, (size_t)length);
	if (spins > 0) {
		/* The parked thread may own one of them; its waiters spin. */
		int extra = snprintf(record, sizeof(record),
			"<0> linuxu FATAL #%u: %ld spinlock(s) held at park time\n",
			nth, spins);
		if (extra > 0 && (size_t)extra < sizeof(record))
			klog_write(record, (size_t)extra);
	}
	fprintf(stderr, "linuxu FATAL #%u: %s at %s:%d (spinlocks held: %ld)\n",
		nth, why ? why : "?", file ? file : "?", line, spins);
}

#if LINUXU_FATAL_PARK
static __attribute__((noreturn)) void fatal_park(void)
{
	__atomic_add_fetch(&fatal_parked_threads, 1, __ATOMIC_ACQ_REL);
	for (;;) {
		/* Sleeps (DriverKit: IOSleep), never spins; loop on early return. */
		struct timespec interval = { .tv_sec = 3600, .tv_nsec = 0 };
		(void)nanosleep(&interval, NULL);
	}
}
#endif

void linuxu_fatal(const char *why, const char *file, int line)
{
	unsigned int nth = __atomic_add_fetch(&fatal_entries, 1, __ATOMIC_ACQ_REL);
	int slot = fatal_claim_reporter((uintptr_t)pthread_self());

	if (slot >= 0) {
		linuxu_fatal_hook_t hook;

		/* Report before containment so the reason reaches the retained
		 * ring even if the hook itself faults or never returns. */
		fatal_report(nth, why, file, line);
		hook = __atomic_load_n(&fatal_hook, __ATOMIC_ACQUIRE);
		if (hook)
			hook(why, file, line);
		__atomic_store_n(&fatal_reporters[slot], 0, __ATOMIC_RELEASE);
	}
#if LINUXU_FATAL_PARK
	fatal_park();
#else
	abort();
#endif
}
