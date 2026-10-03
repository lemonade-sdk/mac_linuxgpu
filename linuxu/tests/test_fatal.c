/* linuxu_fatal(): record, contain through the registered hook, then park the
 * calling thread without spinning. Built twice: with LINUXU_FATAL_PARK=1
 * (dext behaviour) and without it (host behaviour: abort / trap). */
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <rt/fatal.h>
#include <rt/klog.h>

static void pause_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static int klog_contains(const char *needle)
{
	static char text[LINUXU_KLOG_CAPACITY + 1];
	uint64_t cursor = 0;
	size_t length = klog_read(&cursor, text, LINUXU_KLOG_CAPACITY, NULL);
	text[length] = '\0';
	return strstr(text, needle) != NULL;
}

#if LINUXU_FATAL_PARK
static unsigned int hook_calls;
static int hook_line;
static char hook_why[32];
static int hook_reenters;

static void hook(const char *why, const char *file, int line)
{
	(void)file;
	__atomic_add_fetch(&hook_calls, 1, __ATOMIC_ACQ_REL);
	hook_line = line;
	snprintf(hook_why, sizeof(hook_why), "%s", why);
	/* A fault inside containment must park, not recurse. */
	if (__atomic_load_n(&hook_reenters, __ATOMIC_ACQUIRE))
		linuxu_fatal("nested", "hook.c", 99);
}

static void wait_parked(unsigned int count)
{
	for (int i = 0; i < 5000 && linuxu_fatal_parked() < count; ++i)
		pause_ms(1);
	assert(linuxu_fatal_parked() == count);
}

static double cpu_seconds(void)
{
	struct rusage usage;
	assert(!getrusage(RUSAGE_SELF, &usage));
	return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
	       usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

static DEFINE_SPINLOCK(held_lock);

static void *fatal_holding_spinlock(void *unused)
{
	(void)unused;
	spin_lock(&held_lock);
	linuxu_fatal("unit fatal", "unit.c", 12);
}

static void *fatal_reentering(void *unused)
{
	(void)unused;
	linuxu_fatal("reentrant", "unit.c", 34);
}

static void *bug_thread(void *unused)
{
	(void)unused;
	BUG();
	return NULL;
}

static void *panic_thread(void *unused)
{
	(void)unused;
	panic("boom %d", 7);
}

static void spawn(void *(*fn)(void *))
{
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, fn, NULL));
	assert(!pthread_detach(thread));
}

int main(void)
{
	linuxu_fatal_set_hook(hook);

	/* Record, contain, park; the held spinlock is reported. */
	spawn(fatal_holding_spinlock);
	wait_parked(1);
	assert(hook_calls == 1 && hook_line == 12 && !strcmp(hook_why, "unit fatal"));
	assert(klog_contains("linuxu FATAL #1: unit fatal at unit.c:12"));
	assert(klog_contains("1 spinlock(s) held at park time"));
	assert(spin_is_locked(&held_lock));

	/* Parked threads sleep: the process stays nearly idle. */
	double before = cpu_seconds();
	pause_ms(300);
	assert(cpu_seconds() - before < 0.1);
	assert(linuxu_fatal_parked() == 1);

	/* A fatal from inside the hook parks that thread without recursion. */
	__atomic_store_n(&hook_reenters, 1, __ATOMIC_RELEASE);
	spawn(fatal_reentering);
	wait_parked(2);
	assert(hook_calls == 2 && hook_line == 34);
	assert(linuxu_fatal_count() == 3);
	assert(!klog_contains("nested at hook.c"));
	__atomic_store_n(&hook_reenters, 0, __ATOMIC_RELEASE);

	/* BUG() and panic() take the same path. */
	spawn(bug_thread);
	wait_parked(3);
	assert(hook_calls == 3 && !strcmp(hook_why, "BUG"));
	spawn(panic_thread);
	wait_parked(4);
	assert(hook_calls == 4 && !strcmp(hook_why, "panic"));
	assert(klog_contains("Kernel panic - not syncing: boom 7"));

	puts("linuxu_fatal: reported, contained once per thread, parked without spinning");
	return 0; /* process exit reclaims the parked threads */
}
#else
static int child_signal(void (*fn)(void))
{
	pid_t pid = fork();
	int status = 0;
	assert(pid >= 0);
	if (!pid) {
		/* Keep sanitizer and crash reporter noise out of the expected death. */
		fn();
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

static void call_fatal(void) { linuxu_fatal("host", "host.c", 1); }
static void call_bug(void) { BUG(); }
static void call_panic(void) { panic("host panic"); }
static void call_macro(void) { LINUXU_FATAL("host trap"); }

int main(void)
{
	assert(child_signal(call_fatal) == SIGABRT);
	assert(child_signal(call_bug) == SIGABRT);
	assert(child_signal(call_panic) == SIGABRT);
	assert(child_signal(call_macro) != 0);
	puts("linuxu_fatal host build: abort/trap semantics kept");
	return 0;
}
#endif
