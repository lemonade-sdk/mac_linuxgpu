/* GPU recovery's platform side (rt/recovery.h). */
#include <pthread.h>

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/gpu_scheduler.h>
#include <rt/device_string.h>
#include <rt/recovery.h>

#include "amdgpu.h"
#include "amdgpu_reset.h"
#include "kfd_priv.h"

extern int amdgpu_gpu_recovery;

#define WEDGE_TICK_MS	5u
/* amdgpu_device.c's bound on SR-IOV reset retries, applied to bare metal. */
#define RECOVERY_ASIC_RESETS	2u

static pthread_mutex_t recovery_lock = PTHREAD_MUTEX_INITIALIZER;
static struct amdgpu_device *attached;
static struct rt_recovery_state state;
static void (*notify_fn)(const struct rt_recovery_state *state);
static struct task_struct *halt_thread;

/* Each ring's scheduler ops and ring functions as upstream set them, and
 * the copies with the wrapped handlers installed in their place. */
static const struct drm_sched_backend_ops *upstream_sched_ops[AMDGPU_MAX_RINGS];
static struct drm_sched_backend_ops wrapped_sched_ops[AMDGPU_MAX_RINGS];
static const struct amdgpu_ring_funcs *upstream_ring_funcs[AMDGPU_MAX_RINGS];
static struct amdgpu_ring_funcs wrapped_ring_funcs[AMDGPU_MAX_RINGS];
static work_func_t upstream_kfd_reset, upstream_userq_reset;
static const struct amdgpu_asic_funcs *upstream_asic_funcs;
static struct amdgpu_asic_funcs wrapped_asic_funcs;

/* The platform's say on device resets (rt_recovery_set_full_reset_gate). */
static const char *(*full_reset_gate)(void);
/* ASIC resets in the current recovery episode (wrapped_asic_reset). */
static unsigned int episode_asic_resets;
#define RESET_SENTINEL	(-0x7fff)	/* reset_res before upstream's recovery runs */

static void publish(void)
{
	struct rt_recovery_state copy;
	void (*fn)(const struct rt_recovery_state *);

	pthread_mutex_lock(&recovery_lock);
	if (attached)
		state.vram_lost = (uint64_t)atomic_read(&attached->vram_lost_counter);
	copy = state;
	fn = notify_fn;
	pthread_mutex_unlock(&recovery_lock);
	if (fn)
		fn(&copy);
}

void rt_recovery_state(struct rt_recovery_state *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&recovery_lock);
	*out = state;
	pthread_mutex_unlock(&recovery_lock);
}

void rt_recovery_set_notify(void (*fn)(const struct rt_recovery_state *state))
{
	pthread_mutex_lock(&recovery_lock);
	notify_fn = fn;
	pthread_mutex_unlock(&recovery_lock);
}

void rt_recovery_set_full_reset_gate(const char *(*gate)(void))
{
	pthread_mutex_lock(&recovery_lock);
	full_reset_gate = gate;
	pthread_mutex_unlock(&recovery_lock);
}

/* Why a device reset may not run now, or NULL. */
static const char *full_reset_refusal(void)
{
	const char *(*gate)(void);

	pthread_mutex_lock(&recovery_lock);
	gate = full_reset_gate;
	pthread_mutex_unlock(&recovery_lock);
	if (!gate)
		return "a device reset is not available on this platform";
	return gate();
}

bool rt_recovery_full_reset_available(void)
{
	return !full_reset_refusal();
}

/* ---- the wedge ---- */

/* What amdgpu_fence_driver_hw_fini does for a device that is gone: every
 * fence emitted on the ring completes with -ECANCELED. */
static void complete_rings(struct amdgpu_device *adev)
{
	for (unsigned int i = 0; i < AMDGPU_MAX_RINGS; ++i) {
		struct amdgpu_ring *ring = adev->rings[i];

		if (!ring || !ring->fence_drv.initialized)
			continue;
		if ((uint32_t)atomic_read(&ring->fence_drv.last_seq) ==
		    READ_ONCE(ring->fence_drv.sync_seq))
			continue;
		amdgpu_fence_driver_force_completion(ring);
	}
}

static int halt_main(void *arg)
{
	struct amdgpu_device *adev = arg;

	while (!kthread_should_stop()) {
		complete_rings(adev);
		msleep(WEDGE_TICK_MS);
	}
	return 0;
}

void rt_recovery_wedge(struct amdgpu_device *adev, const char *why)
{
	struct drm_device *ddev = adev ? adev_to_drm(adev) : NULL;
	bool first;

	if (!adev)
		return;
	pthread_mutex_lock(&recovery_lock);
	first = !(state.flags & RT_RECOVERY_WEDGED);
	if (first) {
		state.flags |= RT_RECOVERY_WEDGED;
		state.generation++;
		state.last_result = -ENODEV;
	}
	pthread_mutex_unlock(&recovery_lock);
	if (!first)
		return;
	/* No further device reset either (amdgpu_device_should_recover_gpu
	 * reads this). */
	amdgpu_gpu_recovery = 0;
	dev_err(adev->dev, "GPU wedged: %s. Every request fails from now on; power-cycle the GPU, "
		"then reconnect it\n", why ? why : "recovery failed");
	/* amdgpu_device_halt's order: the DRM device, then no hardware access
	 * (and no DMA), then the fences; then KFD's waiters learn of it. */
	WRITE_ONCE(ddev->unplugged, true);
	adev->no_hw_access = true;
	if (adev->pdev)
		pci_clear_master(adev->pdev);
	complete_rings(adev);
	pthread_mutex_lock(&recovery_lock);
	if (!halt_thread) {
		halt_thread = kthread_run(halt_main, adev, "amdgpu-wedged");
		if (IS_ERR_OR_NULL(halt_thread))
			halt_thread = NULL;
	}
	pthread_mutex_unlock(&recovery_lock);
	if (!halt_thread)
		dev_err(adev->dev, "GPU wedged: no thread completes later work; waits end at their "
			"deadlines\n");
	if (adev->kfd.dev)
		for (unsigned int i = 0; i < adev->kfd.dev->num_nodes; ++i)
			kfd_signal_reset_event(adev->kfd.dev->nodes[i]);
	publish();
}

void rt_recovery_end(void)
{
	struct task_struct *thread;

	pthread_mutex_lock(&recovery_lock);
	thread = halt_thread;
	halt_thread = NULL;
	pthread_mutex_unlock(&recovery_lock);
	if (thread)
		kthread_stop(thread);
}

/* ---- the wrapped handlers ---- */

static bool queue_reset_available(struct amdgpu_ring *ring)
{
	return upstream_ring_funcs[ring->idx] && upstream_ring_funcs[ring->idx]->reset &&
	       amdgpu_ring_is_reset_type_supported(ring, AMDGPU_RESET_TYPE_PER_QUEUE);
}

/* A queue reset, as upstream's: one that succeeds advances the
 * generation; one that fails wedges the device before upstream escalates
 * to a device reset (it then finds recovery off). */
static int wrapped_ring_reset(struct amdgpu_ring *ring, unsigned int vmid,
			      struct amdgpu_fence *guilty_fence)
{
	int r = upstream_ring_funcs[ring->idx]->reset(ring, vmid, guilty_fence);

	pthread_mutex_lock(&recovery_lock);
	state.last_result = r;
	if (!r) {
		state.generation++;
		state.queue_resets++;
	}
	pthread_mutex_unlock(&recovery_lock);
	if (!r) {
		dev_err(ring->adev->dev, "GPU recovery: queue %s reset; its guilty context was "
			"cancelled, other work goes on\n", ring->name);
		publish();
	} else {
		const char *refusal = full_reset_refusal();

		if (refusal) {
			char why[256];

			snprintf(why, sizeof(why), "the reset of queue %s failed (%d), and no device "
				 "reset: %s", ring->name, r, refusal);
			rt_recovery_wedge(ring->adev, why);
		}
		/* Otherwise upstream resets the device next. */
	}
	return r;
}

/* The ASIC reset itself (mode1 on gfx12), bounded: upstream retries a
 * device reset whose IB tests fail with no limit on bare metal; after
 * RECOVERY_ASIC_RESETS attempts in one recovery the next one fails, so
 * the recovery ends (and the device wedges). */
static int wrapped_asic_reset(struct amdgpu_device *adev)
{
	int r;

	if (++episode_asic_resets > RECOVERY_ASIC_RESETS) {
		dev_err(adev->dev, "GPU recovery: the device reset did not bring the GPU back after "
			"%u attempts; no further attempt\n", RECOVERY_ASIC_RESETS);
		return -ENODEV;
	}
	r = upstream_asic_funcs->reset(adev);
	pthread_mutex_lock(&recovery_lock);
	state.last_result = r;
	pthread_mutex_unlock(&recovery_lock);
	return r;
}

/* Around a recovery of upstream's that may reset the device: the episode's
 * attempts start at zero; afterwards, a device reset that ran either
 * advanced the generation (VRAM lost or not) or failed, which wedges the
 * device (upstream leaves its schedulers stopped and sends no event). */
static void device_reset_begin(struct amdgpu_device *adev)
{
	episode_asic_resets = 0;
	atomic_set(&adev->reset_domain->reset_res, RESET_SENTINEL);
}

static void device_reset_end(struct amdgpu_device *adev, int vram_lost_before)
{
	const int res = atomic_read(&adev->reset_domain->reset_res);
	const bool vram_lost = atomic_read(&adev->vram_lost_counter) != vram_lost_before;

	if (res == RESET_SENTINEL)
		return;	/* no device reset ran (the queue reset sufficed) */
	if (res) {
		char why[96];

		snprintf(why, sizeof(why), "the device reset failed (%d)", res);
		rt_recovery_wedge(adev, why);
		return;
	}
	pthread_mutex_lock(&recovery_lock);
	state.generation++;
	state.last_result = 0;
	if (vram_lost)
		state.flags |= RT_RECOVERY_LAST_VRAM_LOST;
	else
		state.flags &= ~RT_RECOVERY_LAST_VRAM_LOST;
	pthread_mutex_unlock(&recovery_lock);
	dev_err(adev->dev, "GPU recovery: the device was reset%s\n",
		vram_lost ? "; VRAM contents were lost" : "");
	publish();
}

/* A job timeout: upstream's handler. When it would reset the device (no
 * queue reset on this ring) and that is refused, the device wedges instead
 * (recovery is off for upstream's handler, which then only logs). */
static enum drm_gpu_sched_stat wrapped_timedout(struct drm_sched_job *s_job)
{
	struct amdgpu_ring *ring = to_amdgpu_ring(s_job->sched);
	struct amdgpu_device *adev = ring->adev;
	const int saved = amdgpu_gpu_recovery;
	const int vram_lost = atomic_read(&adev->vram_lost_counter);
	const char *refusal = amdgpu_gpu_recovery ? full_reset_refusal() : NULL;
	const bool wedge = refusal && !queue_reset_available(ring);
	enum drm_gpu_sched_stat stat;

	if (wedge)
		amdgpu_gpu_recovery = 0;
	else
		device_reset_begin(adev);
	stat = upstream_sched_ops[ring->idx]->timedout_job(s_job);
	if (wedge) {
		char why[256];

		amdgpu_gpu_recovery = saved;
		snprintf(why, sizeof(why), "a job on %s timed out, the ring has no queue reset, and "
			 "no device reset: %s", ring->name, refusal);
		rt_recovery_wedge(adev, why);
	} else {
		device_reset_end(adev, vram_lost);
	}
	return stat;
}

/* KFD's and the user queues' device-reset requests: upstream's, when a
 * device reset may run; the wedge otherwise. */
static void kfd_reset_work(struct work_struct *work)
{
	struct amdgpu_device *adev = container_of(work, struct amdgpu_device, kfd.reset_work);
	const char *refusal = full_reset_refusal();
	const int vram_lost = atomic_read(&adev->vram_lost_counter);
	char why[256];

	if (refusal) {
		snprintf(why, sizeof(why), "KFD requested a device reset (its queues stopped "
			 "answering), and no device reset: %s", refusal);
		rt_recovery_wedge(adev, why);
		return;
	}
	device_reset_begin(adev);
	upstream_kfd_reset(work);
	device_reset_end(adev, vram_lost);
}

static void userq_reset_work(struct work_struct *work)
{
	struct amdgpu_device *adev = container_of(work, struct amdgpu_device, userq_reset_work);
	const char *refusal = full_reset_refusal();
	const int vram_lost = atomic_read(&adev->vram_lost_counter);
	char why[256];

	if (refusal) {
		snprintf(why, sizeof(why), "a user queue requested a device reset, and no device "
			 "reset: %s", refusal);
		rt_recovery_wedge(adev, why);
		return;
	}
	device_reset_begin(adev);
	upstream_userq_reset(work);
	device_reset_end(adev, vram_lost);
}

int rt_recovery_attach(struct amdgpu_device *adev)
{
	if (!adev)
		return -EINVAL;
	pthread_mutex_lock(&recovery_lock);
	if (attached) {
		pthread_mutex_unlock(&recovery_lock);
		return attached == adev ? 0 : -EBUSY;
	}
	attached = adev;
	memset(&state, 0, sizeof(state));
	pthread_mutex_unlock(&recovery_lock);
	for (unsigned int i = 0; i < AMDGPU_MAX_RINGS; ++i) {
		struct amdgpu_ring *ring = adev->rings[i];

		if (!ring || ring->idx >= AMDGPU_MAX_RINGS)
			continue;
		if (!ring->no_scheduler && ring->sched.ops && ring->sched.ops->timedout_job) {
			upstream_sched_ops[ring->idx] = ring->sched.ops;
			wrapped_sched_ops[ring->idx] = *ring->sched.ops;
			wrapped_sched_ops[ring->idx].timedout_job = wrapped_timedout;
			ring->sched.ops = &wrapped_sched_ops[ring->idx];
		}
		if (ring->funcs && ring->funcs->reset) {
			upstream_ring_funcs[ring->idx] = ring->funcs;
			wrapped_ring_funcs[ring->idx] = *ring->funcs;
			wrapped_ring_funcs[ring->idx].reset = wrapped_ring_reset;
			ring->funcs = &wrapped_ring_funcs[ring->idx];
		}
	}
	if (adev->kfd.reset_work.func) {
		upstream_kfd_reset = adev->kfd.reset_work.func;
		adev->kfd.reset_work.func = kfd_reset_work;
	}
	if (adev->userq_reset_work.func) {
		upstream_userq_reset = adev->userq_reset_work.func;
		adev->userq_reset_work.func = userq_reset_work;
	}
	if (adev->asic_funcs && adev->asic_funcs->reset) {
		upstream_asic_funcs = adev->asic_funcs;
		wrapped_asic_funcs = *adev->asic_funcs;
		wrapped_asic_funcs.reset = wrapped_asic_reset;
		adev->asic_funcs = &wrapped_asic_funcs;
	}
	return 0;
}

void rt_recovery_detach(struct amdgpu_device *adev)
{
	pthread_mutex_lock(&recovery_lock);
	if (!adev || attached != adev) {
		pthread_mutex_unlock(&recovery_lock);
		return;
	}
	attached = NULL;
	pthread_mutex_unlock(&recovery_lock);
	rt_recovery_end();
	for (unsigned int i = 0; i < AMDGPU_MAX_RINGS; ++i) {
		struct amdgpu_ring *ring = adev->rings[i];

		if (!ring || ring->idx >= AMDGPU_MAX_RINGS)
			continue;
		if (upstream_sched_ops[ring->idx] && ring->sched.ops == &wrapped_sched_ops[ring->idx])
			ring->sched.ops = upstream_sched_ops[ring->idx];
		if (upstream_ring_funcs[ring->idx] && ring->funcs == &wrapped_ring_funcs[ring->idx])
			ring->funcs = upstream_ring_funcs[ring->idx];
		upstream_sched_ops[ring->idx] = NULL;
		upstream_ring_funcs[ring->idx] = NULL;
	}
	if (upstream_asic_funcs && adev->asic_funcs == &wrapped_asic_funcs)
		adev->asic_funcs = upstream_asic_funcs;
	upstream_asic_funcs = NULL;
	if (upstream_kfd_reset)
		adev->kfd.reset_work.func = upstream_kfd_reset;
	if (upstream_userq_reset)
		adev->userq_reset_work.func = upstream_userq_reset;
	upstream_kfd_reset = upstream_userq_reset = NULL;
	amdgpu_gpu_recovery = -1;
}

static struct amdgpu_device *adev_of(struct pci_dev *pdev)
{
	struct drm_device *ddev = pdev ? pci_get_drvdata(pdev) : NULL;

	return ddev ? drm_to_adev(ddev) : NULL;
}

int rt_recovery_attach_pdev(struct pci_dev *pdev)
{
	struct amdgpu_device *adev = adev_of(pdev);

	return adev ? rt_recovery_attach(adev) : -ENODEV;
}

void rt_recovery_detach_pdev(struct pci_dev *pdev)
{
	rt_recovery_detach(adev_of(pdev));
}
