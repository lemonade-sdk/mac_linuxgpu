/* Compute power transitions over upstream amdkfd's own PM entry points.
 *
 * Which upstream path, and why
 * ----------------------------
 * Upstream amdgpu has no suspend flavour for a discrete GPU that keeps VRAM:
 * system suspend (amdgpu_pmops_suspend, S3 and s2idle alike), hibernation
 * and runtime PM (amdgpu_pmops_runtime_suspend, with BACO or BAMACO) all run
 * amdgpu_device_prepare() and amdgpu_device_suspend(), and both call
 * amdgpu_device_evict_resources(), which moves every VRAM buffer to system
 * memory through TTM and SDMA. Only APUs skip that eviction. On the iPad
 * (16 GB of RAM behind a 32 GB board holding ~18 GB of model weights) the
 * eviction cannot fit, and on every host it is the SDMA bulk move that has
 * already hung a ring and dropped the GPU off the Thunderbolt bus.
 *
 * What upstream does offer without touching VRAM is the KFD half of those
 * same paths. amdgpu_device_suspend() calls amdgpu_amdkfd_suspend(adev,
 * true), which on a dGPU is kgd2kfd_suspend(kfd, true):
 *   - kgd2kfd_suspend_process(): kfd_suspend_all_processes() evicts every
 *     KFD process's user queues (kfd_process_evict_queues ->
 *     evict_process_queues_cpsch -> remove_queue_mes, MES saving the waves
 *     of a running dispatch through CWSR) and signals each process's
 *     eviction fence; new KFD processes are refused while it holds;
 *   - each node's queue manager stops (stop_cpsch ->
 *     remove_all_kfd_queues_mes), after which no MES queue operation is
 *     issued until it starts again.
 * amdgpu_device_resume() undoes it with amdgpu_amdkfd_resume(adev, true) =
 * kgd2kfd_resume(kfd, true): start_cpsch, then kfd_resume_all_processes()
 * (restore_process_helper: the process's buffers are revalidated in place,
 * a new eviction fence is attached, and restore_process_queues_cpsch maps
 * every queue back through add_queue_mes, resuming saved waves).
 *
 * That is the state these functions put the device in: every user queue
 * unmapped and its work saved, the GPU idle at its SMU-managed idle clocks,
 * VRAM and the GPUVM page tables untouched. It is what a host can keep while
 * the GPU stays powered (an app moving to the background, a locked screen
 * without system sleep, a client asking ahead of a sleep). When the host
 * really sleeps the Thunderbolt link goes down and the endpoint is reset,
 * so VRAM cannot survive it on any of these hosts; the driver then closes
 * the session through the normal upstream removal path and clients reload
 * (see dext/sources/power_state.h).
 *
 * The functions only call the upstream entry points named above; the checks
 * around them read upstream's own state (queue lists, the queue manager's
 * scheduling flag and the per-process reset marks kfd_hws_hang() leaves when
 * MES fails a removal), so a hung scheduler is reported instead of being
 * mistaken for a quiesced one. Every MES operation upstream issues here is
 * bounded by MES's own completion timeout. Callers serialize all calls for
 * one device. Runtime: linuxu/src/amdgpu-rt/power.c. */
#ifndef LINUXU_RT_POWER_H
#define LINUXU_RT_POWER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amdgpu_device;
struct pci_dev;

/* What the device's KFD queue managers hold, read under each node's queue
 * manager lock. */
struct rt_power_report {
	uint32_t nodes;		/* KFD nodes of the device */
	uint32_t running;	/* nodes whose queue manager is scheduling */
	uint32_t processes;	/* KFD processes registered with the nodes */
	uint32_t queues;	/* their user queues */
	uint32_t active;	/* queues still mapped to MES */
	uint32_t evicted;	/* queues marked evicted */
	uint32_t reset_marked;	/* processes kfd_hws_hang() marked (MES failed) */
};

/* Read the report without changing anything. -ENODEV when KFD is not
 * bound to @adev. */
int rt_power_kfd_report(struct amdgpu_device *adev, struct rt_power_report *out);

/* kgd2kfd_suspend(adev->kfd.dev, true). 0 when every queue was unmapped and
 * every queue manager stopped; -EIO when MES failed a removal on the way
 * (a process newly marked for reset) or a queue is still mapped: the GPU
 * may still run that queue, and its memory must not be freed. Either way
 * KFD stays suspended until rt_power_kfd_resume (upstream counts suspends).
 * -EALREADY when already suspended, -ENODEV without KFD. @out (optional)
 * receives the report taken after the suspend. */
int rt_power_kfd_suspend(struct amdgpu_device *adev, struct rt_power_report *out);

/* kgd2kfd_resume(adev->kfd.dev, true). 0 once the queue managers schedule
 * again and every process's queues are restored; upstream's error
 * otherwise (KFD is no longer counted as suspended either way, as upstream
 * decrements its count before restoring). -EALREADY when not suspended. */
int rt_power_kfd_resume(struct amdgpu_device *adev, struct rt_power_report *out);

/* Whether rt_power_kfd_suspend holds a suspend of @adev's KFD. */
int rt_power_kfd_suspended(const struct amdgpu_device *adev);

/* The same three for the device bound to @pdev (the dext's handle):
 * -ENODEV when no amdgpu device is bound. */
int rt_power_quiesce(struct pci_dev *pdev, struct rt_power_report *out);
int rt_power_resume(struct pci_dev *pdev, struct rt_power_report *out);
int rt_power_quiesced(struct pci_dev *pdev);

#ifdef __cplusplus
}
#endif
#endif
