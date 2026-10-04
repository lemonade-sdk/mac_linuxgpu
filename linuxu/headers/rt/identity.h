/* What the upstream driver knows about the board after its probe: the
 * values Linux shows in sysfs and AMDGPU_INFO, copied from the amdgpu
 * device (linuxu/src/amdgpu-rt/identity.c). Nothing here is derived or
 * assumed per board: a value upstream does not have stays empty or zero.
 *
 *   vram_bytes        adev->gmc.real_vram_size (sysfs mem_info_vram_total)
 *   vram_type         adev->gmc.vram_type, AMDGPU_VRAM_TYPE_* (uapi), and
 *   vram_type_name    its name as upstream prints it ("GDDR6")
 *   vram_bit_width    adev->gmc.vram_width
 *   compute_units     adev->gfx.cu_info.number
 *   gc_version        the GC IP version, amdgpu_ip_version(adev, GC_HWIP, 0):
 *                     IP_VERSION(major, minor, rev), major in bits 31:24
 *   gfx_target        the ISA target KFD assigns the device, ROCr's name
 *                     for kfd->device_info.gfx_target_version ("gfx1201");
 *                     empty when KFD does not support the device
 *   vbios_pn          the VBIOS part number (sysfs vbios_version)
 *   vbios_version     the VBIOS version string (atom vbios_ver_str)
 *   vbios_build       sysfs vbios_build
 *   vbios_date        the VBIOS date
 *   product_name      the FRU board name (sysfs product_name); empty on
 *                     boards without a FRU EEPROM
 *
 * Shared with DriverKit and C++ callers: no kernel types. */
#ifndef LINUXU_RT_IDENTITY_H
#define LINUXU_RT_IDENTITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;

#define RT_IDENTITY_VERSION	1u

struct rt_device_identity {
	uint32_t version;		/* RT_IDENTITY_VERSION */
	uint16_t vendor, device;
	uint16_t subsystem_vendor, subsystem_device;
	uint32_t revision;
	uint32_t gc_version;
	uint32_t gfx_target_version;	/* KFD's, 0 without KFD */
	uint32_t vram_type;
	uint32_t vram_bit_width;
	uint32_t compute_units;
	uint64_t vram_bytes;
	uint64_t visible_vram_bytes;
	char gfx_target[16];
	char vram_type_name[16];
	char vbios_pn[64];
	char vbios_version[32];
	char vbios_build[32];
	char vbios_date[32];
	char product_name[64];
};

/* Fill @out from the amdgpu device bound to @pdev. Returns 0, -EINVAL, or
 * -ENODEV when no amdgpu device is bound (no probe, or it was removed). */
int rt_device_identity(struct pci_dev *pdev, struct rt_device_identity *out);

#ifdef __cplusplus
}
#endif
#endif
