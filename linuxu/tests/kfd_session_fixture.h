/* A fixture amdgpu device for upstream KFD tests (kfd_session_fixture.c).
 * C tests see the fixture's state directly; C++ tests use the kernel-free
 * functions. */
#ifndef KFD_SESSION_FIXTURE_H
#define KFD_SESSION_FIXTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEST_GPU_ID		0x51e5
#define TEST_PASID		0x8001	/* the first render open's; +1 per open */
#define TEST_VRAM_START		0x8000000000ULL
#define TEST_VRAM_BYTES		(256ULL << 20)
#define TEST_DOORBELL_BUS	0xfc000000ULL
#define TEST_DOORBELL_BYTES	(2ULL << 20)
#define TEST_KERNEL_GPU_BASE	0xffff800000000000ULL

/* amd_queue_t and AQL packet offsets the fixture's command processor reads. */
#define FIXTURE_AQL_RING_BASE		8	/* hsa_queue.base_address */
#define FIXTURE_AQL_RING_SIZE		24	/* hsa_queue.size (packets) */
#define FIXTURE_AQL_WRITE_ID		56	/* write_dispatch_id */
#define FIXTURE_AQL_READ_ID		128	/* read_dispatch_id */
#define FIXTURE_AQL_QUEUE_BYTES		256
#define FIXTURE_AQL_COMPLETION		56	/* packet completion_signal */
#define FIXTURE_AQL_PACKET_INVALID	1
#define FIXTURE_AQL_PACKET_DISPATCH	2
#define FIXTURE_MES_SLOTS		32	/* ADD_QUEUEs recorded */

#ifdef __cplusplus
extern "C" {
#endif
struct amdgpu_device;
struct rt_compute_ctx;

void fixture_device_init(void);
void fixture_device_fini(void);
/* kfd_init's character device (and debugfs), with the render node. */
void fixture_kfd_init(void);
/* kfd_init's process workqueues. */
void fixture_kfd_wq_init(void);
/* kfd_exit's process half: clean up processes, flush and destroy the
 * workqueues (the KFD process release work runs), then RCU callbacks. */
void fixture_kfd_release_processes(void);
void fixture_kfd_exit(void);
void fixture_cp_start(void);
void fixture_cp_stop(void);
/* Completions of interrupt signals (a nonzero event mailbox) raise the
 * end-of-pipe interrupt (kfd_signal_event_interrupt) unless dropped. */
void fixture_cp_drop_interrupts(bool drop);
unsigned int fixture_cp_interrupts(void);
unsigned int fixture_cp_interrupts_dropped(void);

struct amdgpu_device *fixture_adev(void);
struct rt_compute_ctx *fixture_compute_ctx(void);
size_t fixture_kmalloc_live(void);
unsigned int fixture_mes_adds(void);
unsigned int fixture_mes_removes(void);
/* The device leaves the bus (rt_removal_active) or comes back. */
void fixture_remove_device(bool removed);
/* MES queues whose waves never preempt: REMOVE_QUEUE fails until MES's
 * hung-queue reset reset them. */
void fixture_mes_hang_all(bool hung);
unsigned int fixture_mes_failed_removes(void);
unsigned int fixture_mes_hang_resets(void);
unsigned int fixture_live_bos(void);
unsigned int fixture_kernel_allocs(void);
unsigned int fixture_cp_dispatches(void);
unsigned int fixture_render_balance(void);
uint64_t fixture_doorbell(uint32_t dword_index);
void *fixture_va_to_host(uint32_t pasid, uint64_t va, uint64_t bytes);
uint32_t fixture_pasid_of(uint64_t va);
/* A GPU page fault of process @pasid at @va, delivered to KFD as the IH
 * delivers one (kfd_int_process_v11's ISR, then its work handler). The
 * fixture's CP raises one itself, from a workqueue, when a completion
 * signal it must write is not mapped; that queue then stalls. */
void fixture_vm_fault(uint32_t pasid, uint64_t va, bool write);
/* A faulted queue stays on its HQD after MES removes it (and says so);
 * MES's hung-queue reset takes it off unless @through_mes_reset, when only
 * a GPU reset (amdgpu_amdkfd_gpu_reset) does. fixture_cp_stuck counts them. */
void fixture_cp_fault_sticks(bool sticks, bool through_mes_reset);
unsigned int fixture_cp_stuck(void);
/* GPU resets that wait for fixture_gpu_reset_finish to take stuck queues off. */
void fixture_gpu_reset_defer(bool defer);
void fixture_gpu_reset_finish(void);
unsigned int fixture_vm_faults(void);
/* Prints every BO still alive; returns how many. */
unsigned int fixture_report_bos(void);
#ifdef __cplusplus
}
#else
struct rt_compute_ctx { int bos; };
extern struct rt_compute_ctx compute_ctx;
extern struct amdgpu_device *adev;
extern struct kfd_dev kfd;
extern struct kfd_node node;
extern struct kfd_topology_device topo;
extern uint64_t doorbell_bar[TEST_DOORBELL_BYTES / 8];
extern unsigned int mes_adds, mes_removes;
extern uint32_t mes_doorbells[FIXTURE_MES_SLOTS];
extern uint64_t mes_wptr[FIXTURE_MES_SLOTS], mes_page_table[FIXTURE_MES_SLOTS];
extern uint32_t mes_pasid[FIXTURE_MES_SLOTS];
/* REMOVE_QUEUEs to fail once each (MES timing out; the queue stays). */
extern unsigned int mes_fail_removes;
extern bool fixture_restores_allowed;
extern unsigned int bo_restores;
extern unsigned int live_bos, gart_maps, render_opens, render_releases, vm_acquires;
extern unsigned int kgd_allocs, kgd_frees, kgd_maps, kgd_unmaps, kernel_allocs;
extern unsigned int sdma_copies, sdma_window_copies, mes_shader_debugger_sets, mes_shader_debugger_flushes;
extern unsigned int cp_dispatches, hqd_dumps;
extern size_t kmemcheck_live_bytes(void);
/* MES failure modes (kfd_session_fixture.c). */
extern bool mes_dead;
extern unsigned int mes_remove_delay_us, mes_failed_removes, mes_hang_resets, mes_resumes,
	gpu_reset_requests;
void fixture_mes_hang(uint32_t doorbell, bool hung);
/* An SDMA engine that holds its copies until released. */
extern bool sdma_hold;
void fixture_sdma_release(void);
void fixture_sdma_finish(bool run);
#endif
#endif
