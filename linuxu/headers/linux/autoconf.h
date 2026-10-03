/* linuxu: SHIM — mk/kernel_config.mk documents the same table. */
#ifndef __LINUXU_AUTOCONF_H
#define __LINUXU_AUTOCONF_H
/* static checked-in CONFIG table.  y -> #define ... 1 ; n -> absent. */
/* NOTE: CONFIG_X86 and CONFIG_X86_64 stay UNDEFINED on Apple Silicon — see
 * the X86 override at the bottom. */
#define CONFIG_DRM 1
#define CONFIG_DRM_KMS 1
#define CONFIG_DRM_KMS_HELPER 1
#define CONFIG_DRM_TTM 1
#define CONFIG_DRM_TTM_HELPER 1
#define CONFIG_DRM_SCHED 1
#define CONFIG_DRM_EXEC 1
#define CONFIG_DRM_CLIENT 1
#define CONFIG_DRM_SUBALLOC_HELPER 1
#define CONFIG_DRM_AMDGPU 1
#define CONFIG_DRM_AMDGPU_USERPTR 1
/* Display Core is compiled in; linuxu_driver_bootstrap() keeps it off
 * (amdgpu_dc=0) unless display is requested. CONFIG_DRM_AMD_DC_FP is a
 * declared intervention (patches/manifest.json config_interventions). */
#define CONFIG_DRM_AMD_DC 1
#define CONFIG_DRM_AMD_DC_FP 1
#define CONFIG_DRM_DISPLAY_HELPER 1
#define CONFIG_DRM_DISPLAY_DP_HELPER 1
#define CONFIG_DRM_DISPLAY_DSC_HELPER 1
#define CONFIG_DRM_DISPLAY_HDCP_HELPER 1
#define CONFIG_DRM_DISPLAY_HDMI_HELPER 1
#define CONFIG_HSA_AMD 1
/* No recoverable GPU faults/SVM or peer GPU access over this TB5 path. */
#define CONFIG_64BIT 1
#define CONFIG_SMP 1
#define CONFIG_RCU 1
#define CONFIG_PCI 1
#define CONFIG_I2C 1
#define CONFIG_PM 1
#define CONFIG_MMU_NOTIFIER 1
#define CONFIG_HMM_MIRROR 1
#define CONFIG_ZONE_DEVICE 1
#define CONFIG_MMU 1
#define CONFIG_DMA_BUF 1
#define CONFIG_DEBUG_FS 1
#define CONFIG_SYSFS 1
#define CONFIG_DEV_COREDUMP 1

/* IS_ENABLED()/IS_BUILTIN()/IS_REACHABLE(), as kbuild's forced kconfig.h. */
#include <linux/kconfig.h>

#endif
