/* Explicit init order for the upstream DRM, scheduler, and AMDGPU modules. */
#include <rt/bootstrap.h>
#include <rt/rt.h>
#include <linux/errno.h>
#include <linux/sysinfo.h>
#include <linux/timer.h>

extern int linuxu_module_init_drm_core_init(void);
extern void linuxu_module_exit_drm_core_exit(void);
extern int linuxu_module_init_gpu_buddy_module_init(void);
extern void linuxu_module_exit_gpu_buddy_module_exit(void);
extern int linuxu_module_init_drm_sched_fence_slab_init(void);
extern void linuxu_module_exit_drm_sched_fence_slab_fini(void);
extern int linuxu_module_init_amdgpu_init(void);
extern void linuxu_module_exit_amdgpu_exit(void);
extern int amdgpu_runtime_pm;
extern int amdgpu_ras_enable;
extern unsigned int amdgpu_ras_mask;
extern unsigned int amdgpu_debug_mask;
extern int amdgpu_num_kcq;
extern int amdgpu_rebar;
extern unsigned int amdgpu_pp_feature_mask;
extern int amdgpu_gpu_recovery;
extern int amdgpu_dc;

/* include/amd_shared.h: PP_GFXOFF_MASK in enum PP_FEATURE_MASK. */
#define LINUXU_PP_GFXOFF_MASK 0x8000u

/* amdgpu_drv.c: AMDGPU_DEBUG_USE_VRAM_FW_BUF.  Firmware staged in GTT
 * requires GPU reads from DART-backed host memory, which the Apple Silicon
 * Thunderbolt DART path does not provide reliably; a VRAM staging BO keeps
 * PSP front-door loading on device memory for every ASIC. */
#define LINUXU_AMDGPU_USE_VRAM_FW_BUF (1u << 3)

/* 0 = stopped, 1 = initializing or stopping, 2 = running. */
static int linuxu_bootstrap_state;
/* Display opt-in; read once per bootstrap. */
static int linuxu_display_requested;

int linuxu_driver_set_display(int enable)
{
	int state = __atomic_load_n(&linuxu_bootstrap_state, __ATOMIC_ACQUIRE);

	if (state != 0)
		return -EBUSY;
	__atomic_store_n(&linuxu_display_requested, enable != 0, __ATOMIC_RELEASE);
	return 0;
}

int linuxu_driver_display_enabled(void)
{
	return __atomic_load_n(&linuxu_display_requested, __ATOMIC_ACQUIRE);
}

int linuxu_driver_bootstrap(void)
{
	int expected = 0;
	int ret;

	if (rt_pci_has_retained_probe())
		return -EBUSY;
	if (!__atomic_compare_exchange_n(&linuxu_bootstrap_state, &expected, 1,
					  0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return expected == 2 ? -EALREADY : -EBUSY;

	/* Upstream TTM consumes si_meminfo without checking a return value.
	 * Establish a valid memory snapshot before its first allocation. */
	ret = linuxu_sysinfo_init();
	if (ret) goto fail;

	/* Timer APIs cannot report worker startup failure after probe starts. */
	ret = linuxu_timer_service_init();
	if (ret) goto fail;

	/* Upstream queues on system_*_wq directly; they must exist first. */
	{
		extern int linuxu_workqueue_init(void);
		ret = linuxu_workqueue_init();
		if (ret) goto fail;
	}

	ret = linuxu_module_init_gpu_buddy_module_init();
	if (ret)
		goto fail;

	ret = linuxu_module_init_drm_core_init();
	if (ret)
		goto stop_buddy;

	ret = linuxu_module_init_drm_sched_fence_slab_init();
	if (ret)
		goto stop_drm;

	/* DriverKit has no Linux runtime-PM transition service for a live TB5 GPU. */
	amdgpu_runtime_pm = 0;
	/* macOS owns bridge windows; DriverKit cannot relocate a live PCI BAR. */
	amdgpu_rebar = 0;
	/* The DriverKit platform provides no RAS EEPROM/I2C backend and no RAS
	 * error-reporting interface, so RAS stays off on every device. */
	amdgpu_ras_enable = 0;
	amdgpu_ras_mask = 0;
	amdgpu_debug_mask |= LINUXU_AMDGPU_USE_VRAM_FW_BUF;
	/* No GFXOFF: late init schedules gfx_off_delay_work, which asks the SMU
	 * to power-gate GFX ~100 ms later, before any IB has run. GFXOFF exit
	 * across the Thunderbolt tunnel is unvalidated on this platform for any
	 * ASIC. The default mask keeps the bit; adev->pm.pp_feature is copied
	 * from this global at device init, and the per-generation RLC start
	 * code reads the global directly (as with a ppfeaturemask without
	 * GFXOFF on Linux). Lift once GFXOFF entry/exit is validated over TB. */
	amdgpu_pp_feature_mask &= ~LINUXU_PP_GFXOFF_MASK;
	/* GPU recovery as on Linux (upstream's default): a job that times out
	 * resets its queue (MES queue reset for gfx and compute, SDMA queue
	 * reset), and if that fails the device (amdgpu_device_gpu_recover).
	 * Without it a hung ring's fences never signal and every wait on them
	 * lasts forever. */
	amdgpu_gpu_recovery = -1;
	/* Keep one upstream kernel compute ring; the runtime selects a free
	 * MEC slot after probe instead of colliding with kernel/KIQ queues. */
	amdgpu_num_kcq = 1;
	/* Display is opt-in. amdgpu.dc=0 makes amdgpu_device_has_dc_support()
	 * false, so discovery adds no DM block and no display interrupt,
	 * BIOS-connector or modeset state is created: the same probe as a
	 * build without CONFIG_DRM_AMD_DC. When requested, upstream's default
	 * (-1: DC on every ASIC that has DCN/DCE) applies. */
	amdgpu_dc = linuxu_driver_display_enabled() ? -1 : 0;
	ret = linuxu_module_init_amdgpu_init();
	if (ret)
		goto stop_sched;

	__atomic_store_n(&linuxu_bootstrap_state, 2, __ATOMIC_RELEASE);
	return 0;

stop_sched:
	linuxu_module_exit_drm_sched_fence_slab_fini();
stop_drm:
	linuxu_module_exit_drm_core_exit();
stop_buddy:
	linuxu_module_exit_gpu_buddy_module_exit();
fail:
	__atomic_store_n(&linuxu_bootstrap_state, 0, __ATOMIC_RELEASE);
	return ret;
}

void linuxu_driver_shutdown(void)
{
	int expected = 2;

	/* A failed probe may still own TTM state and callbacks. Its caches and
	 * module globals must outlive those retained device resources. */
	if (rt_pci_has_retained_probe())
		return;

	if (!__atomic_compare_exchange_n(&linuxu_bootstrap_state, &expected, 1,
					  0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;

	linuxu_module_exit_amdgpu_exit();
	linuxu_module_exit_drm_sched_fence_slab_fini();
	linuxu_module_exit_drm_core_exit();
	linuxu_module_exit_gpu_buddy_module_exit();
	__atomic_store_n(&linuxu_bootstrap_state, 0, __ATOMIC_RELEASE);
}
