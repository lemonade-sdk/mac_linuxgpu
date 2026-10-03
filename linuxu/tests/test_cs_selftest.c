/* Kernel-queue command submission offline: the CS self-test
 * (linuxu/src/amdgpu-rt/cs_selftest.c), a render-node client running
 * through the Linux-file transport (lx_files.c), against the unmodified
 * upstream DRM core, GEM, TTM, VM, rings, fences, drm_sched, contexts, CS
 * and syncobjs of the fixture device in cs_fixture.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int usleep(unsigned int usec);
#include <rt/cs_selftest.h>
#include "cs_fixture.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

static const char *const step_names[RT_CS_STEP_COUNT] = {
	"open", "version", "dev_info", "hw_ip", "ctx", "syncobj", "gem_create", "gem_va",
	"gem_mmap", "compute_cs", "compute_wait_cs", "compute_syncobj", "compute_result",
	"sdma_fill", "sdma_copy", "sdma_wait", "sdma_result", "ttm_gtt", "ttm_vram", "teardown",
};

static void report(const struct rt_cs_selftest_result *res)
{
	for (int i = 0; i < RT_CS_STEP_COUNT; ++i)
		fprintf(stderr, "  %-16s %d\n", step_names[i], res->status[i]);
	fprintf(stderr, "  compute seq %llu value 0x%08x fence %u (%llu ns), sdma seq %llu "
		"vram 0x%08x fill 0x%08x (%llu ns), va 0x%llx-0x%llx\n",
		(unsigned long long)res->compute_seq, res->compute_value, res->user_fence,
		(unsigned long long)res->compute_ns, (unsigned long long)res->sdma_seq,
		res->vram_value, res->fill_value, (unsigned long long)res->sdma_ns,
		(unsigned long long)res->va_start, (unsigned long long)res->va_end);
	fprintf(stderr, "  ttm: to GTT moved %llu bytes, read 0x%08x (%llu ns); back moved %llu bytes, "
		"read 0x%08x (%llu ns)\n", (unsigned long long)res->gtt_moved, res->gtt_value,
		(unsigned long long)res->gtt_ns, (unsigned long long)res->vram_moved,
		res->vram_back_value, (unsigned long long)res->vram_ns);
}

int main(void)
{
	struct pci_dev *pdev;
	/* The BAR covers a quarter of VRAM, as the iPad's covers part of it. */
	cs_fixture_visible_vram = 64ULL << 20;
	pdev = cs_fixture_init();
	struct rt_cs_selftest_result res;
	struct cs_fixture_stats before, after;
	int r;

	for (int round = 0; round < 2; ++round) {
		cs_fixture_stats(&before);
		r = rt_cs_selftest_run(pdev, &res);
		report(&res);
		CHECK(r == 0 && res.failed_step == RT_CS_STEP_COUNT);
		CHECK(res.passed == (1u << RT_CS_STEP_COUNT) - 1);
		CHECK(res.family == 152 /* AMDGPU_FAMILY_GC_12_0_0 */ && res.device_id == 0x7551);
		CHECK(res.compute_rings == 1 && res.sdma_rings == 1);
		CHECK(res.compute_value == 0xc0de0001u && res.vram_value == 0xc0de0002u &&
		      res.fill_value == 0x5eed5eedu && res.user_fence == (uint32_t)res.compute_seq);
		/* The VRAM buffer went to GTT through TTM (a GART window) and
		 * its copy from there still holds what it held. */
		CHECK(res.version == 2 && res.steps == RT_CS_STEP_COUNT);
		CHECK(res.gtt_moved >= 64 * 1024 && res.gtt_value == 0xc0de0002u &&
		      res.vram_back_value == 0xc0de0002u);
		cs_fixture_stats(&after);
		/* One compute IB with two WRITE_DATA; the user's two SDMA IBs
		 * and the kernel's (page tables, the cleared VRAM buffer). */
		CHECK(after.compute_ibs - before.compute_ibs == 1);
		CHECK(after.write_data - before.write_data == 2);
		CHECK(after.sdma_ibs - before.sdma_ibs > 2);
		CHECK(after.pte_writes > before.pte_writes);
		CHECK(after.fills - before.fills >= 2 && after.copies - before.copies >= 1);
		CHECK(after.vm_flushes > before.vm_flushes);
		CHECK(after.interrupts > before.interrupts);
		CHECK(after.faults == 0 && after.dart_faults == 0);
	}

	ttm_evict_check();

	/* A compute queue that does not run: the wait times out, the rest is
	 * skipped, and the test's process is kept (tearing it down would wait
	 * for the job) until the job completes. */
	cs_fixture_hold_compute(1);
	r = rt_cs_selftest_run(pdev, &res);
	report(&res);
	CHECK(r == -62 /* ETIME */ && res.failed_step == RT_CS_STEP_COMPUTE_WAIT_CS);
	CHECK(res.status[RT_CS_STEP_COMPUTE_CS] == 0 && res.status[RT_CS_STEP_SDMA_FILL] == 1);
	CHECK(res.status[RT_CS_STEP_TEARDOWN] == RT_CS_PARKED);
	CHECK(rt_cs_selftest_parked() == 1 && rt_cs_selftest_reap() == 1);
	cs_fixture_hold_compute(0);
	for (int i = 0; i < 500 && rt_cs_selftest_reap(); ++i)
		usleep(10000);
	CHECK(rt_cs_selftest_parked() == 0);

	cs_fixture_stop();
	cs_fixture_stats(&after);
	printf("PASS cs selftest offline: render node, INFO, ctx, syncobjs, GEM create/VA/mmap, "
	       "AMDGPU_CS on compute and SDMA through drm_sched, WAIT_CS (async), syncobj "
	       "wait and dependency, user fence, teardown; %lu compute IBs, %lu SDMA IBs, "
	       "%lu PTEs written, %lu interrupts\n", after.compute_ibs, after.sdma_ibs,
	       after.pte_writes, after.interrupts);
	return 0;
}
