/* test_queue_partition: upstream gfx_v12_0.c's MQD registration, so the test
 * checks the DriverKit MQD against amdgpu's real kernel compute queue MQD
 * builder (gfx_v12_0_compute_mqd_init) and the register field masks the
 * upstream gc headers define. Only the code reachable from these entry
 * points is linked. */
/* Upstream REG_SET_FIELD() shifts 1 into bit 31 (KMD_QUEUE) as int, which
 * the kernel build tolerates; keep UBSan's shift check for the code under
 * test only. */
#pragma clang attribute push (__attribute__((no_sanitize("shift"))), apply_to = function)
#include "gfx_v12_0.c"
#pragma clang attribute pop

void test_gfx_v12_0_set_mqd_funcs(struct amdgpu_device *adev)
{
	gfx_v12_0_set_mqd_funcs(adev);
}

const uint32_t test_gfx_v12_doorbell_en = CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK;
const uint32_t test_gfx_v12_doorbell_bif_drop = CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_BIF_DROP_MASK;
const uint32_t test_gfx_v12_qswitch_mode = CP_HQD_PERSISTENT_STATE__QSWITCH_MODE_MASK;
const uint32_t test_gfx_v12_kernel_queue =
	CP_HQD_PQ_CONTROL__PRIV_STATE_MASK | CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK;
const uint32_t test_gfx_v12_aql_pq_control = CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR_MASK |
	CP_HQD_PQ_CONTROL__SLOT_BASED_WPTR_MASK | CP_HQD_PQ_CONTROL__QUEUE_FULL_EN_MASK;
const uint32_t test_gfx_v12_tmpring[4] = {
	COMPUTE_TMPRING_SIZE__WAVES_MASK, COMPUTE_TMPRING_SIZE__WAVES__SHIFT,
	COMPUTE_TMPRING_SIZE__WAVESIZE_MASK, COMPUTE_TMPRING_SIZE__WAVESIZE__SHIFT,
};
