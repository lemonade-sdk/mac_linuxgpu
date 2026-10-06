/* The CP write-pointer polling experiment (rt/wptr_poll.h). */
#include <linux/kernel.h>
#include <linux/printk.h>
#include <rt/removal.h>
#include <rt/wptr_poll.h>
#include "amdgpu.h"
#include "soc15_common.h"
#include "gc/gc_12_0_0_offset.h"
#include "gc/gc_12_0_0_sh_mask.h"

static uint32_t wptr_poll_period;
static bool wptr_poll_on;
static uint32_t wptr_poll_seen = UINT32_MAX;

void rt_wptr_poll_configure(uint32_t period)
{
	__atomic_store_n(&wptr_poll_period, period > 255 ? 255 : period, __ATOMIC_RELEASE);
}

bool rt_wptr_poll_active(void)
{
	return __atomic_load_n(&wptr_poll_on, __ATOMIC_ACQUIRE);
}

static void wptr_poll_note(const char *when, uint32_t value)
{
	pr_info("wptr poll: CP_PQ_WPTR_POLL_CNTL %#x %s (EN %u, PERIOD %u)\n", value, when,
		(value & CP_PQ_WPTR_POLL_CNTL__EN_MASK) ? 1u : 0u,
		(value & CP_PQ_WPTR_POLL_CNTL__PERIOD_MASK) >> CP_PQ_WPTR_POLL_CNTL__PERIOD__SHIFT);
}

void rt_wptr_poll_observe(struct amdgpu_device *adev)
{
	const uint32_t period = __atomic_load_n(&wptr_poll_period, __ATOMIC_ACQUIRE);
	uint32_t value;

	if (!adev || adev->no_hw_access || rt_removal_active(adev) ||
	    amdgpu_ip_version(adev, GC_HWIP, 0) < IP_VERSION(12, 0, 0) ||
	    amdgpu_ip_version(adev, GC_HWIP, 0) >= IP_VERSION(12, 1, 0))
		return;
	value = RREG32_SOC15(GC, 0, regCP_PQ_WPTR_POLL_CNTL);
	if (value != wptr_poll_seen) {
		wptr_poll_note(wptr_poll_seen == UINT32_MAX ? "after the first queue was mapped" :
			       "changed since the last queue was mapped", value);
		wptr_poll_seen = value;
	}
	if (!period) {
		__atomic_store_n(&wptr_poll_on, false, __ATOMIC_RELEASE);
		return;
	}
	if (!(value & CP_PQ_WPTR_POLL_CNTL__EN_MASK) ||
	    ((value & CP_PQ_WPTR_POLL_CNTL__PERIOD_MASK) >> CP_PQ_WPTR_POLL_CNTL__PERIOD__SHIFT) != period) {
		value &= ~CP_PQ_WPTR_POLL_CNTL__PERIOD_MASK;
		value |= (period << CP_PQ_WPTR_POLL_CNTL__PERIOD__SHIFT) & CP_PQ_WPTR_POLL_CNTL__PERIOD_MASK;
		value |= CP_PQ_WPTR_POLL_CNTL__EN_MASK;
		WREG32_SOC15(GC, 0, regCP_PQ_WPTR_POLL_CNTL, value);
		value = RREG32_SOC15(GC, 0, regCP_PQ_WPTR_POLL_CNTL);
		wptr_poll_note("written (MacLinuxGPUWptrPollPeriod)", value);
		wptr_poll_seen = value;
	}
	__atomic_store_n(&wptr_poll_on,
			 (value & CP_PQ_WPTR_POLL_CNTL__EN_MASK) &&
			 ((value & CP_PQ_WPTR_POLL_CNTL__PERIOD_MASK) >> CP_PQ_WPTR_POLL_CNTL__PERIOD__SHIFT) == period,
			 __ATOMIC_RELEASE);
}
