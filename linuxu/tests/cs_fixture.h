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
	unsigned long dart_faults;		/* ... to system memory not DMA-mapped */
};

/* Bring up the linked DRM/amdgpu modules and the fixture device, and
 * register its DRM device (render node minor 128). Aborts on failure. */
struct pci_dev *cs_fixture_init(void);
/* Stop the software engines. */
void cs_fixture_stop(void);
void cs_fixture_stats(struct cs_fixture_stats *out);
/* Make the compute engine ignore its ring until resumed (a hung queue). */
void cs_fixture_hold_compute(int hold);
/* The same for the SDMA engine (TTM's moves, clears and PTE uploads). */
void cs_fixture_hold_sdma(int hold);
/* CPU-visible VRAM (the BAR), set before cs_fixture_init; 0: all VRAM. */
extern uint64_t cs_fixture_visible_vram;
struct amdgpu_device;
struct amdgpu_device *cs_fixture_adev(void);
/* TTM moves VRAM <-> GTT through the upstream code on this device
 * (ttm_evict_check.c). */
void ttm_evict_check(void);

#endif
