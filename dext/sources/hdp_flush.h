/* Client stores into the GPU's BARs on the submission path (from build
 * MLG_BAR_WRITES_BUILD): kernel arguments written into CPU-visible VRAM and
 * published with the HDP flush register, as ROCr and HRX do on Linux.
 *
 * On Linux a ROCr process maps two things for that from KFD:
 *   - the MMIO remap page (KFD_MMAP_TYPE_MMIO): the 4 KiB page of the
 *     register BAR the NBIO remaps HDP_MEM_FLUSH_CNTL and HDP_REG_FLUSH_CNTL
 *     into (kfd_ioctl.h's KFD_MMIO_REMAP_* offsets), reported as
 *     HSA_AMD_AGENT_INFO_HDP_FLUSH;
 *   - VRAM buffers it allocated with CPU access, through the BAR, write
 *     combined, after hsa_amd_agents_allow_access grants the CPU agent.
 *
 * Here a session client on the KFD path asks for both with QueryInfo
 * MLG_QUERY_BAR_WRITES (one scalar in, MLG_BAR_WRITES_WORDS out). The
 * driver then
 *   - gives the client its gate (doorbell_gate.h, MLG_DOORBELL_GATE_MEMORY_TYPE)
 *     if it has none yet, published, so every power transition, device
 *     reset, quarantine, transport fault, removal and close closes it and
 *     waits for the client's brackets before the device stops answering;
 *   - makes the HDP flush page mappable as MLG_HDP_FLUSH_MEMORY_TYPE (one
 *     host page of the register BAR; map it uncached, kIOMapInhibitCache);
 *   - lets the client allocate VRAM with a VA in its host window (BOAlloc
 *     flag DEXT_COMPUTE_BO_FLAG_HOSTABLE) and map such a buffer with BOMap,
 *     which pins it in the CPU-visible window first (rt_kfd_bo_make_cpu_visible);
 *     map it write combined at its VA (kIOMapWriteCombineCache).
 * A client that never asks gets neither, so every store a client makes
 * into VRAM or the HDP page comes from one that brackets them:
 *
 *	if (!mlg_bar_write_begin(gate))
 *		... closed: nothing written; wait or fail (the runtime's job)
 *	memcpy kernel arguments into the mapped VRAM ring;
 *	dmb oshst; *mem_flush = 1; (void)*mem_flush;	// HDP publication
 *	doorbell (mlg_doorbell_ring, a nested bracket);
 *	mlg_bar_write_end(gate);
 *
 * When it learns the device is going (a call answered kIOReturnNoDevice, the
 * power state LOST, its gate closed for good) the client maps anonymous
 * memory over all of it (hsa/src/bar_mapping_retire.h).
 *
 * Out words: [0] version (1), [1] bytes of the HDP flush page mapping,
 * [2] HDP_MEM_FLUSH_CNTL's byte offset in it, [3] HDP_REG_FLUSH_CNTL's,
 * [4] the BAR it lies on, [5] the CPU-visible VRAM window's size. */
#ifndef MAC_LINUXGPU_HDP_FLUSH_H
#define MAC_LINUXGPU_HDP_FLUSH_H

#include <stdint.h>

#define MLG_BAR_WRITES_BUILD 265u
#define MLG_QUERY_BAR_WRITES 0x4c424152ULL	/* "LBAR" */
#define MLG_BAR_WRITES_WORDS 6u
#define MLG_BAR_WRITES_VERSION 1u
#define MLG_HDP_FLUSH_MEMORY_TYPE 0x4846u	/* 'HF' */

enum mlg_bar_writes_word {
	MLG_BAR_WRITES_VERSION_WORD = 0,
	MLG_BAR_WRITES_PAGE_BYTES = 1,
	MLG_BAR_WRITES_MEM_FLUSH = 2,
	MLG_BAR_WRITES_REG_FLUSH = 3,
	MLG_BAR_WRITES_BAR = 4,
	MLG_BAR_WRITES_VISIBLE_VRAM = 5,
};

#endif
