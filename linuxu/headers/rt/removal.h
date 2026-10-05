/* Surprise removal of the GPU (a Thunderbolt unplug): the device left the
 * bus while the driver, its sessions and clients still run.
 *
 * Linux handles this in two halves. The PCI core marks the device
 * disconnected (pci_dev_set_disconnected), so pci_dev_is_disconnected()
 * holds and amdgpu's removal takes its hot-unplug branches; then it calls
 * amdgpu_pci_remove, which unplugs the DRM device first (drm_dev_unplug:
 * drm_dev_enter() fails from then on, so upstream skips every guarded
 * hardware access, GART table writes among them), finishes the hardware
 * without waiting for it (amdgpu_fence_driver_hw_fini force-completes every
 * ring's fences with -ECANCELED instead of waiting), tears KFD down early
 * (amdgpu_amdkfd_device_fini_sw) and unmaps MMIO.
 *
 * In the dext the sessions and client processes still have to close before
 * amdgpu_pci_remove runs, and they wait on GPU work: fences of submitted
 * jobs, MES acknowledgements, copies. rt_removal_begin puts the device in
 * the state the Linux removal path establishes, from the start:
 *   - the PCI device is marked disconnected (linuxu_pci_mark_removed);
 *   - adev->no_hw_access: register and doorbell accesses are skipped, as
 *     amdgpu_device_halt does;
 *   - the DRM device counts as unplugged for drm_dev_enter, so guarded
 *     paths skip the hardware and render-node ioctls fail with -ENODEV;
 *   - every ring's fences are force-completed with -ECANCELED, as
 *     amdgpu_fence_driver_hw_fini does for an unplugged device, and keep
 *     being so (a removal thread, every RT_REMOVAL_TICK_MS) for work
 *     submitted while the sessions close: no wait for the GPU outlives the
 *     removal, and a MES request fails at once instead of after its timeout.
 * A device that is gone cannot reach host memory: nothing the sessions
 * kept for a GPU that might still run needs keeping once removal began
 * (rt_removal_active). rt_removal_end stops the removal thread; call it
 * after the sessions closed and before amdgpu_pci_remove (whose own
 * hardware finish completes what is left).
 *
 * Runtime: linuxu/src/amdgpu-rt/removal.c. */
#ifndef LINUXU_RT_REMOVAL_H
#define LINUXU_RT_REMOVAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct amdgpu_device;

#define RT_REMOVAL_TICK_MS 5u

/* Begin the removal of the device bound to @pdev (or of @pdev alone when
 * no amdgpu device is bound). Idempotent; 0, or -ENOMEM when the removal
 * thread could not start (fences are still force-completed once). */
int rt_removal_begin(struct pci_dev *pdev);
/* The device bound to @pdev no longer answers this driver (a definite PCI
 * transport fault closed admission) while it may still be on the bus: as
 * for a removal, the DRM device counts as unplugged, hardware access stops
 * and every ring's fences are force-completed with -ECANCELED from now on
 * (the removal thread), so no wait for GPU work blocks forever. Unlike a
 * removal the PCI device is not marked disconnected (rt_removal_active
 * stays false): the GPU may still reach host memory, so what it was given
 * stays retained. Idempotent; any thread; rt_removal_end stops it. */
int rt_device_lost(struct pci_dev *pdev, const char *why);
/* rt_device_lost for the device the runtime serves now (none: 0). */
int rt_device_lost_active(const char *why);
/* Stop the removal thread (fences are completed a last time). */
void rt_removal_end(void);
/* Whether @adev's device was removed (rt_removal_begin, or a device that no
 * longer answers configuration reads). */
bool rt_removal_active(struct amdgpu_device *adev);
/* Fence completions the removal forced so far (diagnostics, tests). */
unsigned long rt_removal_completions(void);

#ifdef __cplusplus
}
#endif
#endif
