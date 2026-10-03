# mk/host_clang.mk
#
# Host (desktop, non-DriverKit) compile settings for the KMD + shim sources.
# Plain clang, no kernel fixdep magic. `make all` and `make test` reuse
# INCPATHS / HOSTCFLAGS from here — single source of truth.

CC := clang

# -Wno-incompatible-function-pointer-types: clang-21 false positive.
# The BIN_ATTR/DEVICE_ATTR initializers (amdgpu_device.c:295,
# amdgpu_ras.c:2302, drm_sysfs.c:325) trigger
# -Wincompatible-function-pointer-types even though the initializer's
# function type and the struct bin_attribute.read/write member type are
# provably identical: the error's two 'aka' expansions are byte-identical
# (both `long (*)(struct file *, struct kobject *, const struct bin_attribute *,
# char *, long, unsigned long)`), and `clang -E` shows exactly one `loff_t`
# typedef in the preprocessed stream (linuxu/headers/linux/types.h:
# `typedef long loff_t`). The type flows through the BIN_ATTR macro and
# clang-21 fails to unify the two `ssize_t`-spelled function-pointer types
# (ssize_t == long on arm64; ABI is identical, so this is a pure
# misdiagnosis, not a real mismatch). Downgrading at the build level keeps
# the upstream sources byte-identical — the shim/build fix is the only option.
# -ftrivial-auto-var-init=zero: Linux's CONFIG_INIT_STACK_ALL_ZERO (the
# default with any compiler that has it), so local variables start zeroed as
# in a Linux build. Upstream relies on it: amdgpu_vm_bo_update() returns its
# uninitialized r for a mapping-less PRT update (the first AMDGPU_CS of every
# render client).
HOSTCFLAGS := -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 -DLINUXU_RT_HOST_SHADOW=1 -include linux/autoconf.h -ftrivial-auto-var-init=zero -w -MMD -MP -Wno-incompatible-function-pointer-types

# Include order matters: linuxu/headers first (shadow headers override),
# then the driver-local include roots inside the submodule. Never add
# third_party/linux/include: the upstream kernel headers are replaced by
# linuxu/headers. The list mirrors the -I$(FULL_AMD_PATH)/... lines of
# drivers/gpu/drm/amd/amdgpu/Makefile. Paths are spelled out literally
# because several test scripts read this assignment.
INCPATHS := \
	-Ilinuxu/headers \
	-Ithird_party/linux/drivers/gpu/drm/amd/include/asic_reg \
	-Ithird_party/linux/drivers/gpu/drm/amd/include \
	-Ithird_party/linux/drivers/gpu/drm/amd/amdgpu \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/smu11 \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/smu12 \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/smu13 \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/smu14 \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/swsmu/smu15 \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/powerplay/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/powerplay/smumgr \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/powerplay/hwmgr \
	-Ithird_party/linux/drivers/gpu/drm/amd/pm/legacy-dpm \
	-Ithird_party/linux/drivers/gpu/drm/amd/acp/include \
	-Ithird_party/linux/drivers/gpu/drm/amd/display \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/include \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/amdgpu_dm \
	-Ithird_party/linux/drivers/gpu/drm/amd/amdkfd \
	-Ithird_party/linux/drivers/gpu/drm/amd/ras/ras_mgr \
	-Ithird_party/linux/drivers/gpu/drm/amd/ras/rascore \
	-Ithird_party/linux/drivers/gpu/drm/ttm \
	-Ithird_party/linux/drivers/gpu/drm/scheduler \
	-Ithird_party/linux/drivers/gpu/drm

# Depfiles (-MMD -MP) are included at the bottom of the top Makefile.
