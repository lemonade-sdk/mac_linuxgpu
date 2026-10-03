#ifndef LINUXU_RT_FATAL_H
#define LINUXU_RT_FATAL_H

/* Terminal failure path for BUG(), panic() and impossible platform failures.
 *
 * A DriverKit dext must not die on purpose: every dext exit goes through
 * the PCI family's client crash recovery, which has panicked the host.
 * linuxu_fatal() instead records the reason in the retained printk ring,
 * calls the containment hook the platform registered (the dext closes PCI
 * admission and quarantines DMA backing), and then parks the calling
 * thread forever in a sleeping loop. Other threads keep running; the
 * session is expected to be torn down by the owner, not by process exit.
 *
 * LINUXU_FATAL_PARK selects that behaviour. It defaults to on for code
 * compiled for the DriverKit target and off elsewhere, so host unit tests
 * that link single translation units keep their trap/abort death checks.
 * A build may define it explicitly (the linuxu_fatal unit test does). */
#ifndef LINUXU_FATAL_PARK
#if defined(__ENVIRONMENT_DRIVERKIT_VERSION_MIN_REQUIRED__)
#define LINUXU_FATAL_PARK 1
#else
#define LINUXU_FATAL_PARK 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Runs once per fatal call before the thread parks. It may be called from
 * any thread, with arbitrary locks held, so it must only touch cached state
 * (no RPC drains, no allocation, no blocking). */
typedef void (*linuxu_fatal_hook_t)(const char *why, const char *file, int line);

void linuxu_fatal_set_hook(linuxu_fatal_hook_t hook);
/* Never returns. Parks when LINUXU_FATAL_PARK is set, otherwise aborts. */
void linuxu_fatal(const char *why, const char *file, int line)
	__attribute__((noreturn, cold));
/* Diagnostics: total fatal entries and threads currently parked. */
unsigned int linuxu_fatal_count(void);
unsigned int linuxu_fatal_parked(void);

#ifdef __cplusplus
}
#endif

/* Use at former __builtin_trap() sites. The host-only branch keeps the
 * exact trap semantics without a link dependency on linuxu_fatal(). */
#if LINUXU_FATAL_PARK
#define LINUXU_FATAL(why) linuxu_fatal((why), __FILE__, __LINE__)
#else
#define LINUXU_FATAL(why) ((void)(why), __builtin_trap())
#endif

#endif
