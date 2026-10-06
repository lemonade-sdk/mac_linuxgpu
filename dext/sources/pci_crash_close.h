#ifndef MAC_LINUXGPU_PCI_CRASH_CLOSE_H
#define MAC_LINUXGPU_PCI_CRASH_CLOSE_H

/* A dext process that ends with its PCI session still open panics the Mac.
 * IOUserServer::clientClose sends the provider ClientCrashed(); with the
 * session open, IOPCIDevice::ClientCrashed_Impl runs
 * IOPCIBridge::childClientCrashRecoveryGated, whose BAR-restore loop indexes
 * configEntry->ranges[] with (register - 0x10) >> 2. For the expansion-ROM
 * entry of IODeviceMemory (register 0x30) that is 8, the bridge
 * prefetchable-window slot, which is NULL on an endpoint (the ROM is slot 6),
 * and range->start is read through it: a kernel data abort at
 * IOPCIFamily(2.9)+0x19b5c (IOPCIFamily-726, macOS 26.6), the panics of
 * 2026-10-01 and 2026-10-06. IOPCIFamily assigns a ROM range to every
 * display-class function that has a ROM BAR, so any GPU dext that dies with
 * its session open panics, whatever it did before.
 *
 * With the session closed, ClientCrashed_Impl skips the recovery. So a signal
 * that would end the process first closes the session (bus mastering and
 * memory decode go off in IOPCIDevice::handleClose, as the recovery would
 * have done), then takes the signal's default action, so the crash is still
 * the crash and is still reported. The close must not take a lock the
 * failing thread may hold; the owner hands it in. SIGKILL cannot be caught:
 * a dext must still never be killed.
 *
 * DriverKit links a dext against DriverKit.framework only, which exports no
 * signal call (no sigaction, kill or pthread_kill), so the handler is
 * installed with the system calls themselves, as libsystem's sigaction and
 * _sigtramp make them (xnu bsd/dev/arm/unix_signal.c: the trampoline gets
 * the catcher, the info style, the signal, siginfo, the ucontext and the
 * sigreturn token in x0-x5, and returns through sigreturn). Only signals
 * whose disposition is the default are taken over. arm64 only. */

#include <stdint.h>

#if defined(__arm64__) || defined(__aarch64__)
#define DEXT_CRASH_CLOSE_SUPPORTED 1

namespace dext_crash_close_detail {

enum : long { SYS_getpid_ = 20, SYS_kill_ = 37, SYS_sigaction_ = 46, SYS_sigreturn_ = 184 };
enum : int {
	SIGHUP_ = 1, SIGINT_ = 2, SIGQUIT_ = 3, SIGILL_ = 4, SIGTRAP_ = 5, SIGABRT_ = 6,
	SIGEMT_ = 7, SIGFPE_ = 8, SIGKILL_ = 9, SIGBUS_ = 10, SIGSEGV_ = 11, SIGSYS_ = 12,
	SIGTERM_ = 15, SIGXCPU_ = 24, SIGXFSZ_ = 25,
};
enum : int { SA_SIGINFO_ = 0x40, UC_FLAVOR_ = 30, SI_USER_ = 0x10001 };

using catcher_t = void (*)(int, void *, void *);
using tramp_t = void (*)(catcher_t, int, int, void *, void *, uintptr_t);

/* struct __sigaction (to the kernel) and struct sigaction (from it). */
struct kernel_sigaction {
	catcher_t catcher;
	tramp_t tramp;
	uint32_t mask;
	int flags;
};
struct user_sigaction {
	catcher_t catcher;
	uint32_t mask;
	int flags;
};

/* A BSD system call: the result, or -errno. */
static inline long syscall3(long number, long a, long b, long c)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x16 __asm__("x16") = number;
	long failed;
	__asm__ volatile("svc #0x80\n\tcset %[failed], cs"
			 : "+r"(x0), "+r"(x1), [failed] "=r"(failed)
			 : "r"(x2), "r"(x16)
			 : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
			   "x13", "x14", "x15", "x17", "memory", "cc");
	return failed ? -x0 : x0;
}

/* Signals whose default action ends the process (SIGKILL cannot be caught). */
static const int k_signals[] = {
	SIGHUP_, SIGINT_, SIGQUIT_, SIGILL_, SIGTRAP_, SIGABRT_, SIGEMT_, SIGFPE_,
	SIGBUS_, SIGSEGV_, SIGSYS_, SIGTERM_, SIGXCPU_, SIGXFSZ_,
};

/* One translation unit per process includes this (dext_main.mm; the test). */
static void (*g_close)(void);
static int g_state;	/* 0 armed, 1 closing, 2 closed */
static uint32_t g_armed;	/* bit n: signal n is taken over */

static void trampoline(catcher_t catcher, int, int sig, void *info, void *uctx, uintptr_t token)
{
	catcher(sig, info, uctx);
	syscall3(SYS_sigreturn_, (long)uctx, UC_FLAVOR_, (long)token);
	/* sigreturn does not come back unless refused: end the process. */
	for (;;)
		syscall3(SYS_kill_, syscall3(SYS_getpid_, 0, 0, 0), SIGKILL_, 0);
}

static void handler(int sig, void *info, void *)
{
	int expected = 0;
	if (__atomic_compare_exchange_n(&g_state, &expected, 1, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (void (*close)(void) = __atomic_load_n(&g_close, __ATOMIC_ACQUIRE))
			close();
		__atomic_store_n(&g_state, 2, __ATOMIC_RELEASE);
	} else {
		/* Another thread is closing: die only once the session is closed. */
		while (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != 2)
			__asm__ volatile("yield");
	}
	/* The default action from now on. A fault (si_code from the kernel)
	 * recurs when the interrupted instruction runs again, and is reported
	 * as itself; a sent signal (kill, abort) is sent again, and is taken
	 * as this handler returns (it is blocked until then). */
	const kernel_sigaction dfl = {};
	syscall3(SYS_sigaction_, sig, (long)&dfl, 0);
	const int code = info ? static_cast<const int *>(info)[2] : SI_USER_;
	if (code <= 0 || code >= 0x10000)
		syscall3(SYS_kill_, syscall3(SYS_getpid_, 0, 0, 0), sig, 0);
}

}	// namespace dext_crash_close_detail

/* Arm once per process; later calls only replace the close. 0 when every
 * signal left at its default action is armed, else the first signal the
 * kernel refused (its handler stays as it was). */
static int dext_crash_close_install(void (*close)(void))
{
	using namespace dext_crash_close_detail;
	static int installed;
	__atomic_store_n(&g_close, close, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&installed, 1, __ATOMIC_ACQ_REL))
		return 0;
	for (int sig : k_signals) {
		user_sigaction old = {};
		if (syscall3(SYS_sigaction_, sig, 0, (long)&old) < 0)
			return sig;
		if (old.catcher)	/* ignored or handled already: left alone */
			continue;
		const kernel_sigaction action = { &handler, &trampoline, 0, SA_SIGINFO_ };
		if (syscall3(SYS_sigaction_, sig, (long)&action, 0) < 0)
			return sig;
		__atomic_or_fetch(&g_armed, 1u << sig, __ATOMIC_RELEASE);
	}
	return 0;
}

/* Bit n set: signal n closes the session before it ends the process. */
static uint32_t dext_crash_close_armed(void)
{
	return __atomic_load_n(&dext_crash_close_detail::g_armed, __ATOMIC_ACQUIRE);
}

#else
#define DEXT_CRASH_CLOSE_SUPPORTED 0
static int dext_crash_close_install(void (*)(void)) { return -1; }
static uint32_t dext_crash_close_armed(void) { return 0; }
#endif
#endif
