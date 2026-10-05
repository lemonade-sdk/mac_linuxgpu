/* GPU recovery's platform side.
 *
 * Upstream recovers a GPU job that times out the way Linux does
 * (amdgpu_gpu_recovery, bootstrap.c): its queue is reset first (a MES or
 * SDMA queue reset, amdgpu_job_timedout -> ring->funcs->reset), and if
 * that fails, or the ring has no queue reset, the device
 * (amdgpu_device_gpu_recover: a mode1 reset on gfx12). KFD and user queues
 * request device resets too (kfd.reset_work, userq_reset_work).
 *
 * A device reset needs platform work this runtime does not do yet over
 * Thunderbolt (the PCI configuration's restore with its BARs, MSI-X,
 * client BAR mappings during the reset window). Until it does
 * (rt_recovery_full_reset_available), every request for one wedges the
 * device instead, which is what Linux ends in when a reset fails:
 *   - the DRM device is unplugged for drm_dev_enter, so new requests fail
 *     with -ENODEV;
 *   - no hardware access (adev->no_hw_access), bus mastering off;
 *   - every ring's fences complete with -ECANCELED, and keep completing
 *     (the halt thread), so no wait on the GPU outlives the wedge;
 *   - the state says wedged until the GPU is power-cycled.
 * Queue resets run as upstream runs them, and each one that succeeds
 * advances the reset generation.
 *
 * rt_recovery_attach hooks this in after the probe: each scheduler's
 * timeout handler and each queue reset are wrapped (upstream's handlers
 * run unchanged inside), and the device-reset works are redirected to the
 * wedge. Every recovery handler runs on the reset domain's one ordered
 * work queue, so they never run concurrently. Runtime:
 * linuxu/src/amdgpu-rt/recovery.c. */
#ifndef LINUXU_RT_RECOVERY_H
#define LINUXU_RT_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amdgpu_device;

#define RT_RECOVERY_WEDGED		(1u << 0)	/* power-cycle the GPU */
#define RT_RECOVERY_LAST_VRAM_LOST	(1u << 1)

struct rt_recovery_state {
	uint64_t generation;	/* resets completed (queue or device), and the wedge */
	uint64_t queue_resets;	/* queue resets that succeeded */
	uint64_t vram_lost;	/* upstream's vram_lost_counter */
	uint32_t flags;		/* RT_RECOVERY_* */
	int32_t last_result;	/* the last reset's result (0, or -errno) */
};

/* Hook recovery in for @adev (after a successful probe). 0 or -errno. */
int rt_recovery_attach(struct amdgpu_device *adev);
/* Undo it (before the driver is removed). */
void rt_recovery_detach(struct amdgpu_device *adev);
/* The same for the amdgpu device bound to @pdev (the driver's side). */
struct pci_dev;
int rt_recovery_attach_pdev(struct pci_dev *pdev);
void rt_recovery_detach_pdev(struct pci_dev *pdev);
/* The state, from any thread (it never blocks). */
void rt_recovery_state(struct rt_recovery_state *out);
/* Whether a device reset may run (false: such a request wedges). */
bool rt_recovery_full_reset_available(void);
/* Wedge @adev now (above), with @why logged. Idempotent. */
void rt_recovery_wedge(struct amdgpu_device *adev, const char *why);
/* Called once per change of the state (a reset, the wedge), on the
 * thread that changed it; NULL to stop. The driver publishes it. */
void rt_recovery_set_notify(void (*fn)(const struct rt_recovery_state *state));
/* Stop the wedge's halt thread (with the session's close). */
void rt_recovery_end(void);

#ifdef __cplusplus
}
#endif
#endif
