
CONFIG_DRM := y                      # Root gate for all drm-*.o and amdgpu.o.
CONFIG_DRM_KMS := y                  # amdgpu is a KMS driver (amdgpu_drv.c, amdgpu_device.c, amdgpu_kms.c).
CONFIG_DRM_KMS_HELPER := y           # amdgpu_display.c: drm_atomic_helper_shutdown, drm_fb_helper_gem_is_fb. Must stay y.
CONFIG_DRM_TTM := y                  # ttm/ dir; amdgpu_ttm.c is the BO backend.
CONFIG_DRM_TTM_HELPER := y           # amdgpu_gem.c / amdgpu_ttm.c use drm_gem_ttm_helper.
CONFIG_DRM_SCHED := y                # scheduler/ dir; amdgpu_sched.c, amdgpu_job.c use gpu_sched.
CONFIG_DRM_EXEC := y                 # ttm_bo.c / amdgpu_ttm.c call drm_exec_* for reservation locking.
CONFIG_DRM_CLIENT := y               # amdgpu_drv.c: drm_client_setup; the KFD drm_client is the compute path. Must stay y.
CONFIG_DRM_FBDEV_EMULATION := n      # Compute-only DriverKit service has no framebuffer console.
CONFIG_DRM_AMD_DC := y               # Display Core + amdgpu_dm compiled in. Runtime opt-in: bootstrap sets amdgpu_dc=0 unless display is requested.
CONFIG_DRM_AMD_DC_FP := y            # DCN FPU code (dcn10+, DML, DML2). Intervention: Kconfig drops it for clang+arm64 (kernel FPU), declared in patches/manifest.json.
CONFIG_DRM_AMD_DC_SI := n            # Legacy SI DCE6 (DRM_AMDGPU_SI=n).
CONFIG_DRM_AMD_SECURE_DISPLAY := n   # Debugfs CRC-window secure display; needs DMCU firmware support.
CONFIG_DEBUG_KERNEL_DC := n          # kgdb breakpoints in DC asserts.
CONFIG_DRM_DISPLAY_HELPER := y       # drivers/gpu/drm/display/ (drm_display_helper module), selected by DRM_AMDGPU.
CONFIG_DRM_DISPLAY_DP_HELPER := y    # drm_dp_helper, drm_dp_mst_topology, drm_dp_dual_mode_helper.
CONFIG_DRM_DISPLAY_DSC_HELPER := y   # drm_dsc_helper.
CONFIG_DRM_DISPLAY_HDCP_HELPER := y  # drm_hdcp_helper.
CONFIG_DRM_DISPLAY_HDMI_HELPER := y  # drm_hdmi_helper, drm_scdc_helper.
CONFIG_DRM_DISPLAY_DP_TUNNEL := n    # Not selected by amdgpu.
CONFIG_DRM_DISPLAY_DP_AUX_CEC := n   # No CEC adapter service; drm_dp_cec_* are the upstream !CONFIG inlines.
CONFIG_DRM_DISPLAY_DP_AUX_CHARDEV := n # No /dev/drm_dp_auxN nodes.
CONFIG_DRM_BRIDGE_CONNECTOR := n     # Bridge chains are not used by amdgpu.
CONFIG_CEC_CORE := n                 # No CEC adapter framework; cec_notifier_* are the upstream !CONFIG inlines.
CONFIG_CEC_NOTIFIER := n             # See CEC_CORE.
CONFIG_SND_HDA_COMPONENT := n        # No HDA audio component binding (DP/HDMI audio is a separate PCI function).
CONFIG_DRM_AMD_ISP := n              # ISP (MIPI camera). Drops amdgpu_isp.o, isp_v4_1_*.o.
CONFIG_DRM_AMD_ACP := n              # ACP (audio codec). Drops amdgpu_acp.o, acp/acp_hw.o.
CONFIG_DRM_RADEON := n               # Legacy radeon driver.
CONFIG_DRM_GPUVM := n                # Generic GPU VM helper; amdgpu uses amdgpu_amdkfd_gpuvm.c.
CONFIG_DRM_RAS := n                  # Generic DRM RAS netlink; amdgpu has its own ras/ tree.
CONFIG_DRM_WERROR := n               # Don't make warnings fatal in a porting build.
CONFIG_DRM_USE_DYNAMIC_DEBUG := n    # Reduce header complexity.
CONFIG_DRM_DEBUG_MM := n             # Extra TTM/drm_mm validation.
CONFIG_DRM_DEBUG_MODESET_LOCK := n   # Extra modeset lock validation.
CONFIG_DRM_LOAD_EDID_FIRMWARE := n   # No edid_firmware= override; EDID comes from the sink.
CONFIG_DRM_PRIVACY_SCREEN := n       # No laptop privacy-screen providers.
CONFIG_DRM_ACCEL := n                # Not used by amdgpu.
CONFIG_DRM_PANIC := n                # No display panic screen.
CONFIG_DRM_DRAW := n                 # Not needed.
CONFIG_DRM_PANEL := n                # No embedded panels on a dGPU.
CONFIG_DRM_VRAM_HELPER := n          # Not used by amdgpu.
CONFIG_DRM_BUDDY := n                # Not used.
CONFIG_DRM_GPUSVM := n               # Not used.
CONFIG_DRM_SUBALLOC_HELPER := y      # Required by amdgpu_sa.c command-buffer suballocation.
CONFIG_DRM_GEM_DMA_HELPER := n       # Not used.
CONFIG_DRM_GEM_SHMEM_HELPER := n     # Not used.

CONFIG_DRM_AMDGPU := y               # Root gate for amdgpu.o.
CONFIG_DRM_AMDGPU_SI := n            # Legacy SI (Tahiti/Pitcairn). Drops si.o, gmc_v6_0.o, gfx_v6_0.o, ...
CONFIG_DRM_AMDGPU_CIK := n           # Legacy CIK (Bonaire/Hawaii/Kaveri). Drops cik.o, dce_v8_0.o, gfx_v7_0.o, ...
CONFIG_DRM_AMDGPU_USERPTR := y       # Must stay y. amdgpu_ttm.c userptr functions are gated by this.
CONFIG_HSA_AMD := y                  # Must stay y. Gates the amdkfd/ tree + KFD glue. The compute path.
CONFIG_HSA_AMD_SVM := n              # TB5 DriverKit has no recoverable GPU page-fault path.
CONFIG_HSA_AMD_P2P := n              # Single GPU; no peer GPU routing over this TB5 path.
CONFIG_DRM_AMDGPU_WERROR := n        # Don't make warnings fatal.
CONFIG_GCOV_PROFILE_AMDGPU := n      # No code coverage.

CONFIG_X86 := n
CONFIG_X86_64 := n
CONFIG_X86_MCE_AMD := n              # amdgpu_ras.c MCA page-retirement notifier; compiles out cleanly.
CONFIG_X86_PAT := n                  # amdgpu_object.c:588 — with n, WC memory disabled (safe fallback).
CONFIG_MTRR := n                     # Related to X86_PAT.
CONFIG_64BIT := y                    # amdgpu_device.c:755, amdgpu_ttm.c:2105.
CONFIG_SMP := y                      # per-CPU vars, spinlocks, smp_processor_id().
CONFIG_RCU := y                      # kfd_process.c uses synchronize_srcu, hlist_del_rcu.
CONFIG_NUMA := n                     # Single-socket macOS.
CONFIG_ACPI := n                     # No ACPI on macOS. Drops amdgpu_acpi.o.
CONFIG_ACPI_NUMA := n                # No ACPI.
CONFIG_AMD_PMC := n                  # Only in amdgpu_acpi.c (dropped).
CONFIG_PCI := y                      # Must stay y. amdgpu is a PCI driver.
CONFIG_PCIEASPM := n                 # ASPM quirk handling in nbio files. Shim no-ops.
CONFIG_OF := n                       # No device-tree. Drops drm_of.o.
CONFIG_AUXDISPLAY := n               # Not used.
CONFIG_BACKLIGHT := n                # Not used in amdgpu core.
CONFIG_I2C := y                      # Must stay y but shim provides no-op. amdgpu_i2c.c is unconditional.
CONFIG_GPIO := n                     # No GPIO in amdgpu core (only ISP, off).
CONFIG_DEVFREQ := n                  # Not used.
CONFIG_POWER_EVENTS := n             # Not used.
CONFIG_PM := y                       # amdgpu_device.c suspend/resume ops.
CONFIG_PM_SLEEP := n                 # No Linux system sleep notifier service.
CONFIG_SUSPEND := n                  # No system suspend in userspace.
CONFIG_HIBERNATION := n              # Not needed.
CONFIG_MEMHOTPLUG := n               # Not needed.

CONFIG_MMU_NOTIFIER := y             # Must stay y. Struct members in amdgpu_bo and kfd_process.
CONFIG_HMM_MIRROR := y               # Must stay y. amdgpu_hmm.c is Makefile-gated by this.
CONFIG_ZONE_DEVICE := y              # Required by HMM.
CONFIG_TRANSPARENT_HUGEPAGE := n     # drm_gem.c THP hinting. Not needed.
CONFIG_MMU := y                      # drm_gem.c:1322.
CONFIG_DMA_BUF := y                  # Must stay y. amdgpu_dma_buf.c unconditional; drm_prime.c.
CONFIG_DMA_CMA := n                  # Not used.

CONFIG_DEBUG_FS := y                 # Must stay y. 54 files reference it; amdgpu_debugfs.c unconditional.
CONFIG_PROC_FS := n                  # Drops amdgpu_fdinfo.o.
CONFIG_PERF_EVENTS := n              # Drops amdgpu_pmu.o.
CONFIG_SYSFS := y                    # Must stay y. Device model, drm_sysfs.c.
CONFIG_DEV_COREDUMP := y             # amdgpu_dev_coredump.c unconditional.
CONFIG_CRYPTO := n                   # No crypto usage.
CONFIG_REGMAP := n                   # No regmap usage.
CONFIG_ASYNC_TX := n                 # No async_tx usage.
CONFIG_VGA_SWITCHEROO := n           # Drops amdgpu_atpx_handler.o.
CONFIG_COMPAT := n                   # No 32-bit. Drops amdgpu_ioc32.o, drm_ioc32.o.
CONFIG_DYNAMIC_DEBUG := n            # No-op when off.
CONFIG_FAULT_INJECTION := n          # Not needed.
CONFIG_LOCKDEP := n                  # Not needed.
CONFIG_CGROUP_BPF := n               # kfd_topology.c; compiles out.
CONFIG_CGROUP_DEVICE := n            # kfd_topology.c; compiles out.
CONFIG_AGP := n                      # Drops ttm_agp_backend.o.

# Excluded files (doc 02 §2.2) — config-gated to n.
#
# Space-separated basenames, matched as $(basename).c against the find-based
# source lists in the top Makefile. These compile out (build-list exclusions,
# not source patches).
#
# Gates, in order of the list below:
#   amdgpu_acpi                     ACPI=n
#   amdgpu_fdinfo                   PROC_FS=n
#   amdgpu_pmu                      PERF_EVENTS=n
#   amdgpu_atpx_handler             VGA_SWITCHEROO=n
#   amdgpu_ioc32, drm_ioc32         COMPAT=n
#   drm_of                          OF=n
#   cik*, dce_v8_0, gfx_v7_0, uvd_v4_2, vce_v2_0, amdgpu_amdkfd_gfx_v7,
#   kv_dpm, kv_smc                  DRM_AMDGPU_CIK=n (legacy CIK)
#   si*, si_ih, si_dma, gmc_v6_0, gfx_v6_0, dce_v6_0, uvd_v3_1, vce_v1_0,
#   si_dpm, si_smc, dimgrey_cavefish_reg_init   DRM_AMDGPU_SI=n (legacy SI)
#   amdgpu_acp, acp_hw              DRM_AMD_ACP=n
#   amdgpu_isp, isp_v4_1_0, isp_v4_1_1         DRM_AMD_ISP=n
#   ttm_agp_backend                 AGP=n
#   drm_panel                       DRM_PANEL=n
#   drm_panic, drm_privacy_screen*  display configs n
#   drm_fbdev_{ttm,dma,shmem}       fbdev sub-parts not needed
#   drm_gem_vram_helper             DRM_VRAM_HELPER=n
#   drm_buddy                       DRM_BUDDY=n
#   drm_gpuvm                       DRM_GPUVM=n
#   drm_gpusvm, drm_pagemap*        DRM_GPUSVM=n
#   drm_fb_helper                   DRM_FBDEV_EMULATION=n
#   drm_gem_dma_helper              DRM_GEM_DMA_HELPER=n
#   drm_gem_shmem_helper            DRM_GEM_SHMEM_HELPER=n
#   kfd_svm, kfd_migrate            HSA_AMD_SVM=n
#   amdgpu_ras_*, ras_*             the ras/ tree is replaced by
#                                   linuxu/ras_stubs.c

# ---------------------------------------------------------------------------
# EXCLUDED_FILES — basenames NOT in the build manifest.
# ONLY two categories may appear here:
#   1. CONFIG-gated to n (see the CONFIG table above) — upstream never builds them.
#   2. Explicitly deferred to a later milestone: the ras/ tree.
# NEVER exclude a file to make the build pass: if a manifest file fails to
# compile, fix the shim/headers, do not drop the file.
# ---------------------------------------------------------------------------
EXCLUDED_FILES := drm_fb_helper \
	amdgpu_acpi \
	kfd_svm \
	kfd_migrate \
	amdgpu_fdinfo \
	amdgpu_pmu \
	amdgpu_atpx_handler \
	amdgpu_ioc32 \
	cik \
	cik_ih \
	cik_sdma \
	dce_v8_0 \
	gfx_v7_0 \
	uvd_v4_2 \
	vce_v2_0 \
	amdgpu_amdkfd_gfx_v7 \
	dimgrey_cavefish_reg_init \
	si \
	si_ih \
	si_dma \
	gmc_v6_0 \
	gfx_v6_0 \
	dce_v6_0 \
	uvd_v3_1 \
	vce_v1_0 \
	amdgpu_acp \
	acp_hw \
	amdgpu_isp \
	isp_v4_1_0 \
	isp_v4_1_1 \
	ttm_agp_backend \
	drm_ioc32 \
	drm_of \
	amdgpu_ras_cmd \
	amdgpu_ras_eeprom_i2c \
	amdgpu_ras_mgr \
	amdgpu_ras_mp1_v13_0 \
	amdgpu_ras_nbio_v7_9 \
	amdgpu_ras_process \
	amdgpu_ras_sys \
	amdgpu_virt_ras_cmd \
	ras_aca \
	ras_aca_v1_0 \
	ras_cmd \
	ras_core \
	ras_cper \
	ras_eeprom \
	ras_eeprom_fw \
	ras_gfx \
	ras_gfx_v9_0 \
	ras_log_ring \
	ras_mp1 \
	ras_mp1_v13_0 \
	ras_nbio \
	ras_nbio_v7_9 \
	ras_process \
	ras_psp \
	ras_psp_v13_0 \
	ras_umc \
	ras_umc_v12_0
