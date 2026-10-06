/* Every IOPCIDevice call the dext makes is one the kernel guarantees safe.
 *
 * dext/sources/dext_main.mm compiled whole against pci_call_mocks.h, whose
 * IOPCIDevice counts every call and flags any outside the kernel's contract
 * (a closed session, a memory index of no BAR, an access outside its BAR, a
 * Close with a call in flight). Cases:
 *   guards   an invalid memory index (the expansion ROM, index 3, and one
 *            past every entry), accesses outside the token and outside the
 *            BAR, aperture accesses outside BAR0, and every accessor after
 *            Close: refused before any call, a transport fault recorded;
 *   race     Close waits for an admitted access and a raw config read in
 *            flight, and admits nothing new meanwhile;
 *   fatal    a child process ends itself by assert(), std::terminate, a
 *            libc++ hardening assertion and a smashed stack (also with the
 *            PCI control lock held by another thread, and on two threads at
 *            once): its session is closed exactly once before it dies, and
 *            it still dies of SIGABRT; with the session closed normally
 *            first, no second Close (dext/sources/fatal_close.h). */
#include "pci_call_mocks.h"
#include "../../dext/sources/dext_main.mm"
#include <assert.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

/* ---- the seams dext_main.mm links against ---- */
struct mock_slot { bool used; uint8_t index; uint64_t size; };
static mock_slot g_slots[8];
extern "C" uint32_t rt_mmio_mint_token(void *, uint8_t index, uint64_t, uint64_t size, int)
{
	for (uint32_t t = 1; t < 8; t++)
		if (!g_slots[t].used) { g_slots[t] = { true, index, size }; return t; }
	return 0;
}
extern "C" int rt_mmio_slot_info(uint32_t t, uint8_t *index, uint64_t *size)
{
	if (!t || t >= 8 || !g_slots[t].used) return -1;
	*index = g_slots[t].index;
	*size = g_slots[t].size;
	return 0;
}
extern "C" void rt_mmio_free_token(uint32_t t) { if (t < 8) g_slots[t].used = false; }
extern "C" int dext_dma_set_pci(void *) { return 0; }
extern "C" void dext_dma_quarantine(void) {}
extern "C" int dext_dma_begin_reset(void) { return 0; }
extern "C" void dext_dma_end_reset(void) {}
extern "C" int dext_dma_begin_shutdown_reset(void) { return 0; }
extern "C" int dext_dma_end_shutdown_reset(int) { return 0; }
static const struct linuxu_aperture_ops *g_test_aperture_ops;
extern "C" void dext_bar0_set_aperture_ops(const struct linuxu_aperture_ops *ops) { g_test_aperture_ops = ops; }
extern "C" void linuxu_fatal_set_hook(linuxu_fatal_hook_t) {}
extern "C" int printk(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return n;
}
extern "C" int linuxu_rt_inject_irq(int) { return 0; }
extern "C" void linuxu_set_in_interrupt(int) {}

/* ---- helpers ---- */
static IOService g_client;
static uint32_t g_token;

/* The driver never reopens a definite fault (the quarantine owns the
 * device); each case starts from a fresh process state instead. */
static void fresh_state(void)
{
	g_transport_fault = DEXT_PCI_FAULT_NONE;
	g_transport_fault_offset = 0;
	g_pci_access = dext_pci_access_gate();
}

static IOPCIDevice *session(void)
{
	auto *pci = new IOPCIDevice;
	fresh_state();
	CHECK(dext_set_pci(pci, &g_client) == 0);
	CHECK(dext_open(&g_token) == 0 && g_token);
	CHECK(pci->open && pci->violations == 0);
	return pci;
}

static void expect_refused(IOPCIDevice *pci, unsigned calls_before, uint64_t offset)
{
	CHECK(pci->calls == calls_before);
	CHECK(dext_pci_transport_fault() == DEXT_PCI_FAULT_MMIO);
	CHECK(dext_pci_transport_fault_offset() == offset);
	fresh_state();
}

static void guards(void)
{
	IOPCIDevice *pci = session();
	uint32_t v32 = 0;
	uint64_t v64 = 0;

	/* What Open captured: BAR0, BAR2, BAR5 (indices 0-2), not the ROM. */
	CHECK(g_bars[0].present && g_bars[0].index == 0 && g_bars[0].size == 256ull << 20);
	CHECK(g_bars[2].present && g_bars[2].index == 1 && g_bars[2].size == 2ull << 20);
	CHECK(g_bars[5].present && g_bars[5].index == 2 && g_bars[5].size == 512ull << 10);
	CHECK(!g_bars[1].present && !g_bars[3].present && !g_bars[4].present);
	CHECK(g_reg_bar == 5 && g_reg_window == 512ull << 10);

	/* The ordinary access: one kernel call. */
	unsigned before = pci->calls;
	CHECK(dext_mem_read32(g_token, 0x100, &v32) == 0 && v32 == 0x5a5a5a5au);
	CHECK(dext_mem_write64(g_token, 0x108, 1) == 0);
	CHECK(pci->calls == before + 2 && pci->violations == 0);

	/* An invalid memory index: the expansion ROM's (3), and none at all. */
	for (uint8_t index : { (uint8_t)3, (uint8_t)7 }) {
		uint32_t t = rt_mmio_mint_token(pci, index, 0, 128ull << 10, 0);
		before = pci->calls;
		CHECK(dext_mem_read32(t, 0x40, &v32) == -1);
		expect_refused(pci, before, 0x40);
		CHECK(dext_mem_write32(t, 0x40, 0) == -1);
		expect_refused(pci, before, 0x40);
		rt_mmio_free_token(t);
	}

	/* Outside the token's window, and straddling its end. */
	before = pci->calls;
	CHECK(dext_mem_read32(g_token, 512ull << 10, &v32) == -1);
	expect_refused(pci, before, 512ull << 10);
	CHECK(dext_mem_read64(g_token, (512ull << 10) - 4, &v64) == -1);
	expect_refused(pci, before, (512ull << 10) - 4);
	/* A token wider than its BAR: the BAR, as GetBARInfo gave it, bounds it. */
	uint32_t wide = rt_mmio_mint_token(pci, 2, 0, 1ull << 20, 0);
	CHECK(dext_mem_read32(wide, 600ull << 10, &v32) == -1);
	expect_refused(pci, before, 600ull << 10);
	CHECK(dext_mem_read32(wide, 0x10, &v32) == 0 && pci->calls == before + 1);
	rt_mmio_free_token(wide);
	/* Misaligned. */
	before = pci->calls;
	CHECK(dext_mem_write32(g_token, 0x102, 0) == -1);
	expect_refused(pci, before, 0x102);

	/* The VRAM aperture: inside BAR0 one call; outside it refused. */
	CHECK(g_test_aperture_ops == &g_aperture_ops);
	before = pci->calls;
	uint64_t value = 0;
	g_test_aperture_ops->read((256ull << 20) - 8, &value, 8);
	CHECK(pci->calls == before + 1 && value == 0x5a5a5a5a5a5a5a5aull);
	/* A CPU store to a CPU-visible VRAM kernel BO (an MQD made by
	 * amdgpu_bo_create_kernel): BAR0's index, inside the window. */
	g_test_aperture_ops->write(0x123000, &value, 4);
	g_test_aperture_ops->write(0x123008, &value, 8);
	CHECK(pci->calls == before + 3 && pci->violations == 0);
	CHECK(dext_pci_transport_fault() == DEXT_PCI_FAULT_NONE);
	before = pci->calls;
	value = 0;
	g_test_aperture_ops->read(256ull << 20, &value, 4);
	CHECK((uint32_t)value == UINT32_MAX);
	expect_refused(pci, before, 256ull << 20);
	g_test_aperture_ops->write((256ull << 20) - 2, &value, 4);
	expect_refused(pci, before, (256ull << 20) - 2);
	g_test_aperture_ops->write(1ull << 40, &value, 8);
	expect_refused(pci, before, 1ull << 40);

	/* Config space outside 4 KiB. */
	CHECK(dext_pci_config_read32(4096, &v32) == -1 && pci->calls == before);
	CHECK(dext_pci_transport_fault() == DEXT_PCI_FAULT_CONFIG);
	fresh_state();

	/* After Close: nothing reaches the provider. */
	dext_close();
	CHECK(!pci->open && pci->closes == 1 && pci->violations == 0);
	CHECK(!g_bars[0].present && !g_bars[5].present);
	before = pci->calls;
	CHECK(dext_mem_read32(g_token, 0x100, &v32) == -1);
	CHECK(dext_mem_write32(g_token, 0x100, 0) == -1);
	value = 0;
	g_test_aperture_ops->read(0, &value, 8);
	CHECK(value == UINT64_MAX);
	g_test_aperture_ops->write(0, &value, 8);
	CHECK(dext_pci_config_read32(0, &v32) == -1);
	CHECK(dext_pci_config_write32(4, 0) == -1);
	CHECK(dext_pci_device_present() == 1);
	uint64_t size = 0;
	void *descriptor = nullptr;
	CHECK(dext_copy_bar_memory(0, &size, &descriptor) == -1);
	CHECK(pci->calls == before && pci->violations == 0);
	/* A closed session is not a fault. */
	CHECK(dext_pci_transport_fault() == DEXT_PCI_FAULT_NONE);
	printf("PASS PCI call guards: invalid memory index (ROM, none), outside token and BAR, "
	       "misaligned, aperture outside BAR0, config outside 4 KiB, every accessor after "
	       "Close: refused before any kernel call, faults recorded\n");
}

/* Close against a call in flight on another thread. */
static void close_waits_for(IOPCIDevice *pci, const std::function<void()> &call)
{
	std::atomic<bool> entered{false}, release{false};
	pci->during_read32 = [&] {
		entered = true;
		while (!release) std::this_thread::yield();
	};
	std::thread access(call);
	while (!entered) std::this_thread::yield();
	std::atomic<bool> closed{false};
	std::thread closer([&] { dext_close(); closed = true; });
	usleep(50 * 1000);
	CHECK(!closed && pci->closes == 0 && pci->open);
	/* Nothing new is admitted while the close waits. */
	const unsigned memory = pci->memory_calls;
	uint32_t v32 = 0;
	CHECK(dext_mem_read32(g_token, 0x100, &v32) == -1);
	CHECK(pci->memory_calls == memory);
	release = true;
	access.join();
	closer.join();
	CHECK(closed && pci->closes == 1 && !pci->open && pci->violations == 0);
}

static void race(void)
{
	IOPCIDevice *pci = session();
	close_waits_for(pci, [] {
		uint32_t v32 = 0;
		CHECK(dext_mem_read32(g_token, 0x200, &v32) == 0);
	});
	pci = session();
	close_waits_for(pci, [] { CHECK(dext_pci_device_present() == 1); });
	/* A new session after the close works. */
	pci = session();
	uint32_t v32 = 0;
	CHECK(dext_mem_read32(g_token, 0x100, &v32) == 0);
	dext_close();
	CHECK(pci->closes == 1 && pci->violations == 0);
	printf("PASS PCI close fence: Close waits for an admitted access and a raw config read "
	       "in flight, admits nothing new meanwhile, and a new session opens after it\n");
}

/* ---- fatal paths close first, in child processes ---- */
enum fatal_kind { FATAL_ASSERT, FATAL_TERMINATE, FATAL_LIBCXX, FATAL_STACK, FATAL_ASSERT_LOCKED,
		  FATAL_TWO_THREADS, FATAL_AFTER_CLOSE };

/* A real overrun of a protected frame: the canary check at return calls
 * __stack_chk_fail (built with -fstack-protector-strong). */
__attribute__((noinline)) static void smash(size_t n)
{
	char buffer[16];
	volatile char *p = buffer;
	for (size_t i = 0; i < n; i++) p[i] = 0x41;
}

static IOPCIDevice *g_child_pci;
static int g_child_fd;
static void note_close(void)
{
	/* 'C': a Close of the open session, nothing in flight. */
	const char c = g_child_pci->violations ? 'V' : (g_child_pci->open ? 'C' : 'X');
	(void)write(g_child_fd, &c, 1);
}

[[noreturn]] static void run_fatal_child(fatal_kind kind, int fd)
{
	/* No crash report for a death the test asks for. */
	task_set_exception_ports(mach_task_self(), EXC_MASK_CRASH | EXC_MASK_CORPSE_NOTIFY,
				 MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	IOPCIDevice *pci = session();
	g_child_pci = pci;
	g_child_fd = fd;
	/* Reported from inside the provider's Close. */
	pci->during_close = &note_close;
	switch (kind) {
	case FATAL_ASSERT:
		assert(getpid() == 0);
		break;
	case FATAL_TERMINATE:
		std::terminate();
	case FATAL_LIBCXX:
		std::__libcpp_verbose_abort("hardening: index %d out of range", 7);
	case FATAL_STACK:
		smash(64);
		break;
	case FATAL_ASSERT_LOCKED: {
		std::atomic<bool> held{false};
		std::thread([&] {
			dext_pci_control_guard control;
			held = true;
			for (;;) pause();
		}).detach();
		while (!held) std::this_thread::yield();
		assert(getpid() == 0);
		break;
	}
	case FATAL_TWO_THREADS: {
		std::atomic<int> ready{0};
		for (int i = 0; i < 2; i++)
			std::thread([&] {
				ready++;
				while (ready < 2) {}
				assert(getpid() == 0);
			}).detach();
		for (;;) pause();
	}
	case FATAL_AFTER_CLOSE:
		dext_close();
		assert(getpid() == 0);
		break;
	}
	_exit(0);
}

static void fatal_case(fatal_kind kind, const char *name)
{
	int fds[2];
	CHECK(pipe(fds) == 0);
	fflush(nullptr);
	const pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		close(fds[0]);
		run_fatal_child(kind, fds[1]);
	}
	close(fds[1]);
	char notes[8] = {};
	ssize_t n = 0, r;
	while (n < (ssize_t)sizeof(notes) && (r = read(fds[0], notes + n, sizeof(notes) - n)) > 0)
		n += r;
	close(fds[0]);
	int status = 0;
	CHECK(waitpid(child, &status, 0) == child);
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT || n != 1 || notes[0] != 'C')
		fprintf(stderr, "%s: status %#x (signal %d), Close notes \"%.*s\"\n", name, status,
			WIFSIGNALED(status) ? WTERMSIG(status) : 0, (int)n, notes);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	/* Exactly one Close, of an open session with nothing in flight. */
	CHECK(n == 1 && notes[0] == 'C');
}

static void fatal(void)
{
	fatal_case(FATAL_ASSERT, "assert");
	fatal_case(FATAL_TERMINATE, "std::terminate");
	fatal_case(FATAL_LIBCXX, "libc++ verbose abort");
	fatal_case(FATAL_STACK, "smashed stack");
	fatal_case(FATAL_ASSERT_LOCKED, "assert with the PCI control lock held");
	fatal_case(FATAL_TWO_THREADS, "assert on two threads at once");
	fatal_case(FATAL_AFTER_CLOSE, "assert after a normal Close");
	printf("PASS fatal paths close first: assert, std::terminate, a libc++ hardening assertion "
	       "and a smashed stack (also with the PCI control lock held elsewhere, and on two "
	       "threads at once) close the session once, then die of SIGABRT; a session closed "
	       "normally is not closed again\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "guards";
	if (!strcmp(mode, "guards")) guards();
	else if (!strcmp(mode, "race")) race();
	else if (!strcmp(mode, "fatal")) fatal();
	else { fprintf(stderr, "usage: %s guards|race|fatal\n", argv[0]); return 2; }
	return 0;
}
