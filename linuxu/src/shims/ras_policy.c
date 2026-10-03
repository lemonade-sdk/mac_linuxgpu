/* DriverKit compute profile: the unified RAS manager and its EEPROM backends
 * are not linked. Bootstrap forces the upstream ras_enable parameter to 0;
 * these predicates keep optional RAS paths on their unsupported branch. */
#include <amdgpu_ras_mgr.h>

bool amdgpu_uniras_enabled(struct amdgpu_device *adev)
{
	(void)adev;
	return false;
}

bool amdgpu_ras_mgr_check_eeprom_safety_watermark(struct amdgpu_device *adev)
{
	(void)adev;
	/* Upstream returns false when no initialized RAS manager is ready. */
	return false;
}
