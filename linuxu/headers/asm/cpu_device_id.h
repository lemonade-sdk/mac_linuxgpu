/* linuxu: SHIM (vendor/linux/arch/x86/include/asm/cpu_device_id.h) */
#ifndef ASM_CPU_DEVICE_ID_H
#define ASM_CPU_DEVICE_ID_H

#include <linux/types.h>
#include <linux/bits.h>

#define X86_VENDOR_INTEL 0
#define X86_VENDOR_AMD 2

#define VFM_MODEL_BIT	0
#define VFM_FAMILY_BIT	8
#define VFM_VENDOR_BIT	16
#define VFM_RSVD_BIT	24

#define VFM_MODEL_MASK	GENMASK(VFM_FAMILY_BIT - 1, VFM_MODEL_BIT)
#define VFM_FAMILY_MASK	GENMASK(VFM_VENDOR_BIT - 1, VFM_FAMILY_BIT)
#define VFM_VENDOR_MASK	GENMASK(VFM_RSVD_BIT - 1, VFM_VENDOR_BIT)

#define VFM_MODEL(vfm)	(((vfm) & VFM_MODEL_MASK) >> VFM_MODEL_BIT)
#define VFM_FAMILY(vfm)	(((vfm) & VFM_FAMILY_MASK) >> VFM_FAMILY_BIT)
#define VFM_VENDOR(vfm)	(((vfm) & VFM_VENDOR_MASK) >> VFM_VENDOR_BIT)

#define VFM_MAKE(_vendor, _family, _model) (	\
	((_model) << VFM_MODEL_BIT) |		\
	((_family) << VFM_FAMILY_BIT) |		\
	((_vendor) << VFM_VENDOR_BIT)		\
)

/* struct cpuinfo_x86 (vendor 2026 asm/processor.h subset) */
struct cpuinfo_x86 {
	u16		x86;
	u16		x86_vendor;
	u16		x86_model;
	u16		x86_model_id;
};

#endif
