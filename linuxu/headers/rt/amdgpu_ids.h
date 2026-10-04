/* Product names by PCI device and revision, from libdrm's data/amdgpu.ids
 * (MIT): the RDNA4 boards, those a Thunderbolt enclosure takes today. On
 * Linux the kernel driver has no product names; libdrm's
 * amdgpu_get_marketing_name() looks the device up in this file, and Mesa,
 * amdgpu_top and the desktop show what it finds.
 *
 * One table for every user here: libmlg_drm's amdgpu_get_marketing_name()
 * and the dext, which publishes the name for System Information. A device
 * missing from it has no product name (Mesa then says "AMD Unknown"; the
 * dext publishes none).
 *
 * Header-only, no kernel or DriverKit types. */
#ifndef LINUXU_RT_AMDGPU_IDS_H
#define LINUXU_RT_AMDGPU_IDS_H

#include <stddef.h>
#include <stdint.h>

struct amdgpu_ids_entry {
	uint16_t device;
	uint8_t revision;
	const char *name;
};

static const struct amdgpu_ids_entry amdgpu_ids[] = {
	{ 0x7550, 0xc0, "AMD Radeon RX 9070 XT" },
	{ 0x7550, 0xc2, "AMD Radeon RX 9070 GRE" },
	{ 0x7550, 0xc3, "AMD Radeon RX 9070" },
	{ 0x7551, 0xc0, "AMD Radeon AI Pro R9700" },
	{ 0x7551, 0xc1, "AMD Radeon AI Pro R9700S" },
	{ 0x7551, 0xc8, "AMD Radeon AI Pro R9600D" },
	{ 0x7590, 0xc0, "AMD Radeon RX 9060 XT" },
	{ 0x7590, 0xc1, "AMD Radeon RX 9060 XT LP" },
	{ 0x7590, 0xc7, "AMD Radeon RX 9060" },
	{ 0x7590, 0xcf, "AMD Radeon RX 9050" },
	{ 0x7590, 0xdf, "AMD Radeon RX 9050 4GB" },
};

/* The product name of an AMD device (vendor 0x1002), or NULL. */
static inline const char *amdgpu_ids_name(uint16_t device, uint8_t revision)
{
	for (size_t i = 0; i < sizeof(amdgpu_ids) / sizeof(amdgpu_ids[0]); ++i)
		if (amdgpu_ids[i].device == device && amdgpu_ids[i].revision == revision)
			return amdgpu_ids[i].name;
	return NULL;
}

#endif
