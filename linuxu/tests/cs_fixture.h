/* A fixture amdgpu device for kernel-queue command submission tests
 * (cs_fixture.c): the unmodified upstream DRM core, GEM, TTM, VM, rings,
 * fences, drm_sched, contexts, CS and syncobjs over a software GPU that
 * executes what the rings carry. */
#ifndef CS_FIXTURE_H
#define CS_FIXTURE_H
#include <stdint.h>

struct pci_dev;

struct cs_fixture_stats {
	unsigned long compute_ibs, sdma_ibs;	/* IBs executed */
	unsigned long write_data;		/* PM4 WRITE_DATA executed */
	unsigned long fences, interrupts;
	unsigned long pte_writes;		/* SDMA page-table entries written */
	unsigned long fills, copies;		/* SDMA buffer operations */
	unsigned long vm_flushes;
	unsigned long faults;			/* GPU accesses that did not translate */
	unsigned long dma_data;			/* PM4 DMA_DATA (CP DMA) executed */
	unsigned long release_mem;		/* PM4 RELEASE_MEM in IBs executed */
	unsigned long dispatches;		/* dispatch packets seen (not run) */
	unsigned long skipped;			/* packets skipped (state, caches) */
};

/* Bring up the linked DRM/amdgpu modules and the fixture device, and
 * register its DRM device (render node minor 128). Aborts on failure. */
struct pci_dev *cs_fixture_init(void);
/* Stop the software engines. */
void cs_fixture_stop(void);
void cs_fixture_stats(struct cs_fixture_stats *out);
/* Make the compute engine ignore its ring until resumed (a hung queue). */
void cs_fixture_hold_compute(int hold);
/* Let IBs carry what a full driver emits around its work: chained IBs are
 * followed, packets the software GPU does not model (register state,
 * cache and event packets, register writes) are skipped and counted, and
 * dispatches are counted but not run (the software GPU executes no
 * shaders). Off by default, so an unexpected packet aborts. */
void cs_fixture_model_driver_streams(int on);
/* The host memory behind @bytes at @offset of BAR @bar (BAR0: VRAM through
 * its aperture), or NULL: lx_loopback_set_bar_memory's callback. */
void *cs_fixture_bar_memory(uint32_t bar, uint64_t offset, uint64_t bytes);

#endif
