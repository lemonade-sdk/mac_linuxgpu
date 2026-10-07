/* GPU recovery's platform side.
 *
 * Upstream recovers a GPU job that times out the way Linux does
 * (amdgpu_gpu_recovery, bootstrap.c): its queue is reset first (a MES or
 * SDMA queue reset, amdgpu_job_timedout -> ring->funcs->reset), and if
 * that fails, or the ring has no queue reset, the device
 * (amdgpu_device_gpu_recover: a mode1 reset on gfx12). KFD and user queues
 * request device resets too (kfd.reset_work, userq_reset_work).
 *
 * A device reset runs only when the platform allows it
 * (rt_recovery_set_full_reset_gate: the PCI configuration's restore puts
 * the BARs and MSI-X back, and no client may store into a BAR while it
 * stops decoding). It is bounded (AMDGPU_MAX_RETRY_LIMIT ASIC resets per
 * recovery), and one that fails wedges the device, as does every request
 * for one the platform refuses. The wedge is what Linux ends in when a
 * reset fails:
 *   - the DRM device is unplugged for drm_dev_enter, so new requests fail
 *     with -ENODEV;
 *   - no hardware access (adev->no_hw_access), bus mastering off;
 *   - every ring's fences complete with -ECANCELED, and keep completing
 *     (the halt thread), so no wait on the GPU outlives the wedge;
 *   - the state says wedged until the GPU is power-cycled.
 * Queue resets run as upstream runs them; each one that succeeds, each
 * device reset that succeeds (VRAM lost or not), and the wedge advance
 * the reset generation.
 *
 * rt_recovery_attach hooks this in after the probe: each scheduler's
 * timeout handler, each queue reset, the ASIC reset and the KFD and user
 * queue reset works are wrapped (upstream's run unchanged inside). Every recovery handler runs on the reset domain's one ordered
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
/* The platform's say on device resets: @gate returns NULL when one may run
 * now, or why not (logged with the wedge). With no gate (the default) none
 * may. The driver's gate refuses while device resets are not enabled for
 * this device, or while a client maps one of its BARs. */
void rt_recovery_set_full_reset_gate(const char *(*gate)(void));
/* Wedge @adev now (above), with @why logged. Idempotent. */
void rt_recovery_wedge(struct amdgpu_device *adev, const char *why);
/* The driver's own queues (its AQL queues, which MES maps as legacy
 * kernel queues) across a device reset: @before_reset runs as upstream
 * halts the device, before its IPs suspend (the queues go with MES's
 * state, their MQDs may go with VRAM); @after_reset once the reset has
 * succeeded and the schedulers run again (where KFD restores its queues,
 * amdgpu_amdkfd_post_reset), before the new reset generation is
 * published; @vram_lost says whether VRAM survived. Both on the reset
 * domain's thread; neither runs for a queue reset, and @after_reset not
 * for a reset that fails (the device wedges). NULL removes. */
struct rt_recovery_queue_hooks {
	void (*before_reset)(void *arg);
	void (*after_reset)(void *arg, bool vram_lost);
	void *arg;
};
void rt_recovery_set_queue_hooks(const struct rt_recovery_queue_hooks *hooks);
/* Called once per change of the state (a reset, the wedge), on the
 * thread that changed it; NULL to stop. The driver publishes it. */
void rt_recovery_set_notify(void (*fn)(const struct rt_recovery_state *state));
/* Stop the wedge's halt thread (with the session's close). */
void rt_recovery_end(void);

#ifdef __cplusplus
}
#endif
#endif
