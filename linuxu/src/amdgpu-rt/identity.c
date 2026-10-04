/* The board as the upstream driver knows it (rt/identity.h): copies of the
 * amdgpu device's own fields, the ones its sysfs files and AMDGPU_INFO
 * report on Linux. */
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <drm/drm_device.h>
#include <drm/amdgpu_drm.h>
#include <rt/identity.h>

#include "amdgpu.h"
#include "amdgpu_fru_eeprom.h"
#include "atom.h"
#include "kfd_priv.h"

/* amdgpu_vram_names[] (amdgpu_object.c, static there): the names upstream
 * prints for AMDGPU_VRAM_TYPE_*. */
static const char *const vram_names[] = {
	[AMDGPU_VRAM_TYPE_UNKNOWN] = "UNKNOWN",
	[AMDGPU_VRAM_TYPE_GDDR1] = "GDDR1",
	[AMDGPU_VRAM_TYPE_DDR2] = "DDR2",
	[AMDGPU_VRAM_TYPE_GDDR3] = "GDDR3",
	[AMDGPU_VRAM_TYPE_GDDR4] = "GDDR4",
	[AMDGPU_VRAM_TYPE_GDDR5] = "GDDR5",
	[AMDGPU_VRAM_TYPE_HBM] = "HBM",
	[AMDGPU_VRAM_TYPE_DDR3] = "DDR3",
	[AMDGPU_VRAM_TYPE_DDR4] = "DDR4",
	[AMDGPU_VRAM_TYPE_GDDR6] = "GDDR6",
	[AMDGPU_VRAM_TYPE_DDR5] = "DDR5",
	[AMDGPU_VRAM_TYPE_LPDDR4] = "LPDDR4",
	[AMDGPU_VRAM_TYPE_LPDDR5] = "LPDDR5",
	[AMDGPU_VRAM_TYPE_HBM3E] = "HBM3E",
	[AMDGPU_VRAM_TYPE_HBM4] = "HBM4",
};

/* A fixed-size upstream string, without the blanks VBIOS strings are
 * padded with. */
static void text_copy(char *out, size_t size, const void *in, size_t in_size)
{
	size_t n = strnlen(in, in_size);

	if (n >= size)
		n = size - 1;
	memcpy(out, in, n);
	while (n && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\t'))
		--n;
	out[n] = '\0';
}

int rt_device_identity(struct pci_dev *pdev, struct rt_device_identity *out)
{
	struct drm_device *ddev;
	struct amdgpu_device *adev;
	struct atom_context *atom;
	struct kfd_dev *kfd;
	uint32_t target;

	if (!pdev || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->version = RT_IDENTITY_VERSION;
	out->vendor = pdev->vendor;
	out->device = pdev->device;
	out->subsystem_vendor = pdev->subsystem_vendor;
	out->subsystem_device = pdev->subsystem_device;
	out->revision = pdev->revision;
	ddev = pci_get_drvdata(pdev);
	if (!ddev)
		return -ENODEV;
	adev = drm_to_adev(ddev);

	out->gc_version = amdgpu_ip_version(adev, GC_HWIP, 0);
	kfd = adev->kfd.dev;
	target = kfd ? kfd->device_info.gfx_target_version : 0;
	out->gfx_target_version = target;
	/* ROCr's name for a target version: gfx<major><minor><stepping, hex>. */
	if (target)
		snprintf(out->gfx_target, sizeof(out->gfx_target), "gfx%u%u%x",
			 target / 10000, (target / 100) % 100, target % 100);

	out->vram_bytes = adev->gmc.real_vram_size;
	out->visible_vram_bytes = adev->gmc.visible_vram_size;
	out->vram_type = adev->gmc.vram_type;
	out->vram_bit_width = adev->gmc.vram_width;
	if (adev->gmc.vram_type < ARRAY_SIZE(vram_names) && vram_names[adev->gmc.vram_type])
		strscpy(out->vram_type_name, vram_names[adev->gmc.vram_type],
			sizeof(out->vram_type_name));
	out->compute_units = adev->gfx.cu_info.number;

	atom = adev->mode_info.atom_context;
	if (atom) {
		text_copy(out->vbios_pn, sizeof(out->vbios_pn), atom->vbios_pn,
			  sizeof(atom->vbios_pn));
		text_copy(out->vbios_version, sizeof(out->vbios_version), atom->vbios_ver_str,
			  sizeof(atom->vbios_ver_str));
		text_copy(out->vbios_build, sizeof(out->vbios_build), atom->build_num,
			  sizeof(atom->build_num));
		text_copy(out->vbios_date, sizeof(out->vbios_date), atom->date,
			  sizeof(atom->date));
	}
	if (adev->fru_info)
		text_copy(out->product_name, sizeof(out->product_name),
			  adev->fru_info->product_name, sizeof(adev->fru_info->product_name));
	return 0;
}
