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
HOSTCFLAGS := -std=gnu11 -D__KERNEL__ -DCONFIG_DRM_FBDEV_OVERALLOC=0 -DLINUXU_RT_HOST_SHADOW=1 -include linux/autoconf.h -w -MMD -MP -Wno-incompatible-function-pointer-types

# Include order matters: linuxu/headers first (shadow headers override),
# then the driver-local include roots inside the submodule. Never add
# third_party/linux/include: the upstream kernel headers are replaced by
# linuxu/headers. The list mirrors the -I$(FULL_AMD_PATH)/... lines of
# drivers/gpu/drm/amd/amdgpu/Makefile. Paths are spelled out literally
# because several test scripts read this assignment. The display entries
# after drivers/gpu/drm are the subdir-ccflags-y include roots of
# amd/display/Makefile and its dc/ and dml2_0/ Makefiles, in upstream order;
# kbuild applies them to every amdgpu object. (The other display subdir
# flag, -DBUILD_FEATURE_TIMING_SYNC=0, is read by no source at the pin.)
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
	-Ithird_party/linux/drivers/gpu/drm \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/inc/hw \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/clk_mgr \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/hwss \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/resource \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dsc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/optc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dpp \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/hubbub \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dccg \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/hubp \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dio \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dwb \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/hpo \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/mmhubbub \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/mpc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/opp \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/pg \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/soc_and_ip_translator \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/freesync \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/color \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/info_packet \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/power \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dmub/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/modules/hdcp \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0 \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/dml2_core \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/dml2_mcg \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/dml2_dpmm \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/dml2_pmo \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/dml2_standalone_libraries \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/src/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21/inc \
	-Ithird_party/linux/drivers/gpu/drm/amd/display/dc/dml2_0/dml21

# Depfiles (-MMD -MP) are included at the bottom of the top Makefile.
