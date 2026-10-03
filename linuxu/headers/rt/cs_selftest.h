/* Kernel-queue command submission self-test (linuxu/src/amdgpu-rt/
 * cs_selftest.c): what a Vulkan driver (Mesa RADV through libdrm_amdgpu)
 * does on a render node, done by the dext itself through the Linux-file
 * transport (rt/lx_files.h), so it runs the same path a client's calls run.
 *
 * In a Linux process of its own the test opens the render node, reads
 * DRM_IOCTL_VERSION and AMDGPU_INFO (device and hardware IP info),
 * creates a context and two syncobjs, allocates GTT and VRAM buffers,
 * maps them into its GPUVM (GEM_VA) and the GTT ones into its address
 * space (GEM_MMAP + mmap), and then:
 *   compute  one IB of PM4 WRITE_DATA packets writing a value to the GTT
 *            buffer and one to the VRAM buffer, submitted with AMDGPU_CS
 *            on the compute ring with a BO list, a user fence and a
 *            syncobj to signal; waited with AMDGPU_WAIT_CS on a worker
 *            (as the async RPC runs waits) and with DRM_IOCTL_SYNCOBJ_WAIT;
 *   SDMA     an IB the device's own buffer functions build (the packets
 *            TTM uses for clears) filling part of the VRAM buffer, then a
 *            second submission waiting on the first through a syncobj and
 *            copying the VRAM buffer into the GTT buffer;
 *   TTM      the VRAM buffer moved to GTT and back the way a client's
 *            submission moves it (AMDGPU_GEM_OP SET_PLACEMENT, then an SDMA
 *            copy of it whose AMDGPU_CS validates it there): TTM's move
 *            reaches GTT through a GART transfer window, as an eviction
 *            does (window PTEs uploaded by SDMA, VMID 0 flush, copy);
 * and checks every value through its CPU mapping. Everything is undone
 * in reverse, and the process exits.
 *
 * Nothing outside the test's own process is touched: it creates no
 * queue, maps nothing of other clients and holds no lock across a wait.
 * Each wait is bounded (RT_CS_SELFTEST_WAIT_MS); a GPU that never signals
 * fails the step with -ETIME, and a test whose jobs are still running at
 * the end keeps its process until they complete (rt_cs_selftest_reap).
 *
 * Nothing is hard-coded for one GPU: rings, alignment and the GPU VA range
 * come from AMDGPU_INFO, SDMA packets from the device's buffer functions,
 * and the compute packets are PM4 type-3 WRITE_DATA, which every GFX
 * generation with compute rings decodes the same way.
 *
 * Shared with DriverKit and C++ callers: no kernel types. */
#ifndef LINUXU_RT_CS_SELFTEST_H
#define LINUXU_RT_CS_SELFTEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;

#define RT_CS_SELFTEST_VERSION	2u	/* 2: the TTM steps and fields */
#define RT_CS_SELFTEST_WAIT_MS	2000u

enum rt_cs_step {
	RT_CS_STEP_OPEN = 0,		/* render node into a new process */
	RT_CS_STEP_VERSION,		/* DRM_IOCTL_VERSION names "amdgpu" */
	RT_CS_STEP_DEV_INFO,		/* AMDGPU_INFO_DEV_INFO */
	RT_CS_STEP_HW_IP,		/* AMDGPU_INFO_HW_IP_INFO compute + DMA */
	RT_CS_STEP_CTX,			/* AMDGPU_CTX alloc */
	RT_CS_STEP_SYNCOBJ,		/* DRM_IOCTL_SYNCOBJ_CREATE x2 */
	RT_CS_STEP_GEM_CREATE,		/* GTT data, IB and fence; VRAM */
	RT_CS_STEP_GEM_VA,		/* AMDGPU_GEM_VA map */
	RT_CS_STEP_GEM_MMAP,		/* GEM_MMAP + mmap of the GTT buffers */
	RT_CS_STEP_COMPUTE_CS,		/* AMDGPU_CS on the compute ring */
	RT_CS_STEP_COMPUTE_WAIT_CS,	/* AMDGPU_WAIT_CS, on an async worker */
	RT_CS_STEP_COMPUTE_SYNCOBJ,	/* DRM_IOCTL_SYNCOBJ_WAIT on its out syncobj */
	RT_CS_STEP_COMPUTE_RESULT,	/* GTT value and user fence */
	RT_CS_STEP_SDMA_FILL,		/* AMDGPU_CS on DMA: fill VRAM */
	RT_CS_STEP_SDMA_COPY,		/* AMDGPU_CS on DMA after a syncobj: copy */
	RT_CS_STEP_SDMA_WAIT,		/* AMDGPU_WAIT_CS on the copy */
	RT_CS_STEP_SDMA_RESULT,		/* VRAM values seen through GTT */
	RT_CS_STEP_TTM_GTT,		/* VRAM buffer moved to GTT, copied, checked */
	RT_CS_STEP_TTM_VRAM,		/* ... and back to VRAM */
	RT_CS_STEP_TEARDOWN,		/* unmap, close, free, exit */
	RT_CS_STEP_COUNT
};

/* Step status: 0 passed, a negative Linux errno, or one of these. */
#define RT_CS_NOT_RUN		1	/* an earlier step failed */
#define RT_CS_SKIPPED		2	/* the device has no such engine */
#define RT_CS_MISMATCH		3	/* the GPU wrote something else */
#define RT_CS_PARKED		4	/* teardown: a submission is still running */

struct rt_cs_selftest_result {
	uint32_t version;		/* RT_CS_SELFTEST_VERSION */
	uint32_t steps;			/* RT_CS_STEP_COUNT */
	uint32_t passed;		/* bit per step */
	uint32_t failed_step;		/* first failure, or RT_CS_STEP_COUNT */
	int32_t status[24];		/* per step */
	uint32_t family, chip_external_rev, device_id, num_shader_engines;
	uint32_t compute_rings, sdma_rings;	/* available_rings masks */
	uint64_t va_start, va_end;		/* the GPUVM range used */
	uint64_t compute_seq, sdma_seq;		/* AMDGPU_CS sequence numbers */
	uint64_t compute_ns, sdma_ns;		/* submit to fence signaled */
	uint32_t compute_value, vram_value;	/* read back */
	uint32_t fill_value, user_fence;
	/* Version 2: the TTM steps. */
	uint64_t gtt_moved, vram_moved;		/* bytes TTM moved during the step */
	uint64_t gtt_ns, vram_ns;		/* placement to the copy's fence */
	uint32_t gtt_value, vram_back_value;	/* the moved buffer's first word */
};

/* scripts/drm-selftest.py reads this layout. */
#ifdef __cplusplus
static_assert(sizeof(struct rt_cs_selftest_result) == 240, "rt_cs_selftest_result layout");
#else
_Static_assert(sizeof(struct rt_cs_selftest_result) == 240, "rt_cs_selftest_result layout");
#endif

/* Run the test on the GPU bound to @pdev. Returns 0 when every step that
 * applies passed, else the first failing step's status (negative errno,
 * RT_CS_MISMATCH or RT_CS_PARKED); @out holds the details either way.
 * May sleep, at most a few RT_CS_SELFTEST_WAIT_MS. */
int rt_cs_selftest_run(struct pci_dev *pdev, struct rt_cs_selftest_result *out);

/* A test whose submissions had not completed when it ended (a wait timed
 * out) keeps its process: tearing it down would wait for those jobs, as
 * the exit of a Linux process does, and with GPU recovery off a hung job
 * never ends. rt_cs_selftest_reap tears down every kept test whose
 * submissions have completed since and returns how many remain; the GPU
 * must not be removed while any does. */
unsigned int rt_cs_selftest_reap(void);
unsigned int rt_cs_selftest_parked(void);

#ifdef __cplusplus
}
#endif
#endif
