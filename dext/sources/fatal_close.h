#ifndef MAC_LINUXGPU_FATAL_CLOSE_H
#define MAC_LINUXGPU_FATAL_CLOSE_H

/* A dext process that ends with its PCI session open panics the Mac:
 * IOUserServer::clientClose sends the provider ClientCrashed(), and
 * IOPCIBridge::childClientCrashRecoveryGated indexes its BAR ranges with the
 * expansion ROM's register (0x30 -> slot 8, NULL on an endpoint) and reads
 * through NULL at IOPCIFamily(2.9)+0x19b5c. With the session closed first,
 * ClientCrashed_Impl skips that recovery.
 *
 * So every way this process can end itself closes the session first, then
 * goes on to the crash it was going to have:
 *   - assert() (__assert_rtn, defined here, so every reference in the dext
 *     binary and its static library binds to it);
 *   - std::terminate (a handler set before the session can open);
 *   - libc++ hardening (std::__libcpp_verbose_abort, which libc++ lets a
 *     program replace);
 *   - a smashed stack (__stack_chk_fail, defined here: the compiler's
 *     stack-protector calls in this binary bind to it).
 * Each closes, logs one event to the unified log, then abort()s: the crash
 * is still the crash, with its stack.
 *
 * Plain code only: no signal handler, no system call of its own (the
 * DriverKit sandbox kills a dext that calls sigaction, build 259). The
 * close itself is an IOPCIDevice::Close RPC, as a normal close is.
 *
 * Not reachable from here, and so not covered: a hardware fault (EXC_BAD_
 * ACCESS and friends: only a Mach exception handler sees those before the
 * process ends), a libmalloc corruption abort and the libc FORTIFY checks
 * (both abort inside libsystem, whose calls never come through this
 * binary), and SIGKILL from outside (jetsam, launchd, a person). BUG(),
 * panic() and linuxu_fatal() never end the process: they park the thread.
 *
 * The close takes no lock (the failing thread may hold any). The owner
 * registers it; one translation unit per process includes this header
 * (dext_main.mm, and the test that compiles it whole). */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <exception>
#include <__verbose_abort>
#include <os/log.h>

namespace dext_fatal_close_detail {

static void (*g_close)(void);
static int g_state;	/* 0 armed, 1 closing, 2 closed */
static unsigned g_entries;

}	// namespace dext_fatal_close_detail

/* The lock-free session close every fatal path runs first. */
static void dext_fatal_close_register(void (*close)(void))
{
	__atomic_store_n(&dext_fatal_close_detail::g_close, close, __ATOMIC_RELEASE);
}

/* Fatal paths entered so far (tests, diagnostics). */
static unsigned dext_fatal_close_entries(void)
{
	return __atomic_load_n(&dext_fatal_close_detail::g_entries, __ATOMIC_ACQUIRE);
}

/* Close once, whichever thread gets here first; any other waits until the
 * session is closed, so no thread ends the process with it open. */
static void dext_fatal_close_first(void)
{
	using namespace dext_fatal_close_detail;
	__atomic_add_fetch(&g_entries, 1, __ATOMIC_ACQ_REL);
	int expected = 0;
	if (__atomic_compare_exchange_n(&g_state, &expected, 1, false,
					__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (void (*close)(void) = __atomic_load_n(&g_close, __ATOMIC_ACQUIRE))
			close();
		__atomic_store_n(&g_state, 2, __ATOMIC_RELEASE);
		return;
	}
	while (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 2)
		__asm__ volatile("yield");
}

/* Close, record why, then end the process as it was going to end. */
[[noreturn]] __attribute__((cold, noinline)) static void
dext_fatal_die(const char *kind, const char *detail)
{
	dext_fatal_close_first();
	char record[384];
	snprintf(record, sizeof(record),
		 "mac.linuxgpu: EVENT FATAL %s: %s; the PCI session was closed first, the process ends now",
		 kind ? kind : "?", detail ? detail : "?");
	os_log(OS_LOG_DEFAULT, "%{public}s", record);
	abort();
}

static void dext_fatal_terminate_handler(void)
{
	dext_fatal_die("std::terminate", "a C++ terminate (no exception may leave a dext)");
}

/* Before the session can open (a constructor, and dext_set_pci again). */
static void dext_fatal_close_install(void (*close)(void))
{
	dext_fatal_close_register(close);
	std::set_terminate(dext_fatal_terminate_handler);
}

/* assert.h declares it noreturn and cold. */
extern "C" void __assert_rtn(const char *function, const char *file, int line,
					   const char *expression)
{
	char detail[256];
	snprintf(detail, sizeof(detail), "assert(%s) in %s at %s:%d",
		 expression ? expression : "?", function ? function : "?", file ? file : "?", line);
	dext_fatal_die("assert", detail);
}

/* Declared (noreturn) by libc++'s <__verbose_abort>, which lets a program
 * replace it. */
void std::__libcpp_verbose_abort(const char *format, ...) noexcept
{
	char detail[256];
	va_list arguments;
	va_start(arguments, format);
	if (vsnprintf(detail, sizeof(detail), format ? format : "?", arguments) < 0)
		detail[0] = '\0';
	va_end(arguments);
	dext_fatal_die("libc++ assertion", detail);
}

/* The canary of the calling frame is gone; this frame's own is not checked. */
extern "C" [[noreturn]] __attribute__((no_stack_protector)) void __stack_chk_fail(void)
{
	dext_fatal_die("stack smashed", "a stack-protector canary was overwritten");
}

#endif
