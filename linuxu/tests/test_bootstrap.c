#include <assert.h>
#include <linux/errno.h>
#include <string.h>
#include <linux/init.h>
#include <rt/bootstrap.h>

static char trace[32];
static unsigned int trace_len;
static int fail_stage;
static int recursive_result;
static int retained_probe;
static unsigned int memory_init_calls;
int linuxu_sysinfo_init(void)
{
	++memory_init_calls;
	return fail_stage == 7 ? -ENODEV : 0;
}
int rt_pci_has_retained_probe(void) { return retained_probe; }
int amdgpu_runtime_pm = -1;
int amdgpu_ras_enable = -1;
unsigned int amdgpu_ras_mask = ~0U;
unsigned int amdgpu_debug_mask;
int amdgpu_num_kcq;
int amdgpu_rebar = -1;
unsigned int amdgpu_pp_feature_mask = 0xfff7bfff; /* upstream default */
int amdgpu_gpu_recovery = -1;
int amdgpu_dc = -1; /* upstream default */
static int expect_dc;
int linuxu_timer_service_init(void) { return fail_stage == 5 ? -EAGAIN : 0; }
static unsigned int workqueue_init_calls;
int linuxu_workqueue_init(void)
{
	++workqueue_init_calls;
	return fail_stage == 8 ? -EAGAIN : 0;
}

static void event(char c)
{
	assert(trace_len < sizeof(trace) - 1);
	trace[trace_len++] = c;
	trace[trace_len] = '\0';
}

static int gpu_buddy_module_init(void)
{
	event('B');
	return fail_stage == 6 ? -ENOMEM : 0;
}
static void gpu_buddy_module_exit(void) { event('b'); }

static int drm_core_init(void)
{
	event('D');
	if (fail_stage == 4)
		recursive_result = linuxu_driver_bootstrap();
	return fail_stage == 1 ? -EIO : 0;
}
static void drm_core_exit(void) { event('d'); }
static int drm_sched_fence_slab_init(void)
{
	event('S');
	return fail_stage == 2 ? -ENOMEM : 0;
}
static void drm_sched_fence_slab_fini(void) { event('s'); }
static int amdgpu_init(void)
{
	event('A');
	assert(amdgpu_runtime_pm == 0);
	assert(amdgpu_rebar == 0);
	assert(amdgpu_ras_enable == 0);
	assert(amdgpu_ras_mask == 0);
	/* GFXOFF (PP_GFXOFF_MASK) is cleared; other PP features are kept. */
	assert(amdgpu_pp_feature_mask == (0xfff7bfffu & ~0x8000u));
	assert(amdgpu_gpu_recovery == 0);
	assert(amdgpu_dc == expect_dc);
	return fail_stage == 3 ? -ENODEV : 0;
}
static void amdgpu_exit(void) { event('a'); }

module_init(drm_core_init);
module_exit(drm_core_exit);
module_init(gpu_buddy_module_init);
module_exit(gpu_buddy_module_exit);
module_init(drm_sched_fence_slab_init);
module_exit(drm_sched_fence_slab_fini);
module_init(amdgpu_init);
module_exit(amdgpu_exit);

static void reset(int stage)
{
	fail_stage = stage;
	trace_len = 0;
	trace[0] = '\0';
}

int main(void)
{
	assert(trace_len == 0); /* linking the modules does not start them */
	assert(memory_init_calls == 0);
	reset(7);
	assert(linuxu_driver_bootstrap() == -ENODEV);
	assert(memory_init_calls == 1);
	assert(trace_len == 0);
	linuxu_driver_shutdown();
	assert(trace_len == 0);

	reset(5);
	assert(linuxu_driver_bootstrap() == -EAGAIN);
	assert(trace_len == 0);
	assert(workqueue_init_calls == 0);

	/* System workqueues exist before any module init can queue work. */
	reset(8);
	assert(linuxu_driver_bootstrap() == -EAGAIN);
	assert(workqueue_init_calls == 1);
	assert(trace_len == 0);

	reset(6);
	assert(linuxu_driver_bootstrap() == -ENOMEM);
	assert(strcmp(trace, "B") == 0);
	linuxu_driver_shutdown();
	assert(strcmp(trace, "B") == 0);

	reset(1);
	assert(linuxu_driver_bootstrap() == -EIO);
	assert(strcmp(trace, "BDb") == 0);
	linuxu_driver_shutdown();
	assert(strcmp(trace, "BDb") == 0);

	reset(2);
	assert(linuxu_driver_bootstrap() == -ENOMEM);
	assert(strcmp(trace, "BDSdb") == 0);

	reset(3);
	amdgpu_runtime_pm = -1;
	assert(linuxu_driver_bootstrap() == -ENODEV);
	assert(strcmp(trace, "BDSAsdb") == 0);

	/* Display is off unless requested; the request restores upstream's
	 * amdgpu.dc default and cannot change while the modules run. */
	assert(!linuxu_driver_display_enabled());
	reset(3);
	assert(linuxu_driver_set_display(1) == 0);
	assert(linuxu_driver_display_enabled());
	expect_dc = -1;
	assert(linuxu_driver_bootstrap() == -ENODEV);
	assert(linuxu_driver_set_display(0) == 0);
	expect_dc = 0;

	reset(4);
	assert(linuxu_driver_bootstrap() == 0);
	assert(linuxu_driver_set_display(1) == -EBUSY);
	assert(!linuxu_driver_display_enabled());
	assert(recursive_result == -EBUSY);
	assert(strcmp(trace, "BDSA") == 0);
	assert(linuxu_driver_bootstrap() == -EALREADY);
	assert(strcmp(trace, "BDSA") == 0);
	retained_probe = 1;
	unsigned int memory_calls_before_retention = memory_init_calls;
	linuxu_driver_shutdown();
	assert(strcmp(trace, "BDSA") == 0);
	assert(linuxu_driver_bootstrap() == -EBUSY);
	assert(memory_init_calls == memory_calls_before_retention);
	assert(strcmp(trace, "BDSA") == 0);
	retained_probe = 0;
	linuxu_driver_shutdown();
	assert(strcmp(trace, "BDSAasdb") == 0);
	linuxu_driver_shutdown();
	assert(strcmp(trace, "BDSAasdb") == 0);
	return 0;
}
