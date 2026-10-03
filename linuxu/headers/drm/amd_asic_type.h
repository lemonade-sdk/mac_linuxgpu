/* linuxu: AS-IS (third_party/linux/include/drm/amd_asic_type.h) */
#ifndef __AMD_ASIC_TYPE_H__
#define __AMD_ASIC_TYPE_H__

#include <linux/types.h>

/*
 * Supported ASIC types
 */
enum amd_asic_type {
	CHIP_TAHITI = 0,
	CHIP_PITCAIRN,	/* 1 */
	CHIP_VERDE,	/* 2 */
	CHIP_OLAND,	/* 3 */
	CHIP_HAINAN,	/* 4 */
	CHIP_BONAIRE,	/* 5 */
	CHIP_KAVERI,	/* 6 */
	CHIP_KABINI,	/* 7 */
	CHIP_HAWAII,	/* 8 */
	CHIP_MULLINS,	/* 9 */
	CHIP_TOPAZ,	/* 10 */
	CHIP_TONGA,	/* 11 */
	CHIP_FIJI,	/* 12 */
	CHIP_CARRIZO,	/* 13 */
	CHIP_STONEY,	/* 14 */
	CHIP_POLARIS10,	/* 15 */
	CHIP_POLARIS11,	/* 16 */
	CHIP_POLARIS12,	/* 17 */
	CHIP_VEGAM,	/* 18 */
	CHIP_VEGA10,	/* 19 */
	CHIP_VEGA12,	/* 20 */
	CHIP_VEGA20,	/* 21 */
	CHIP_RAVEN,	/* 22 */
	CHIP_ARCTURUS,	/* 23 */
	CHIP_RENOIR,	/* 24 */
	CHIP_ALDEBARAN, /* 25 */
	CHIP_NAVI10,	/* 26 */
	CHIP_CYAN_SKILLFISH,	/* 27 */
	CHIP_NAVI14,	/* 28 */
	CHIP_NAVI12,	/* 29 */
	CHIP_SIENNA_CICHLID,	/* 30 */
	CHIP_NAVY_FLOUNDER,	/* 31 */
	CHIP_VANGOGH,	/* 32 */
	CHIP_DIMGREY_CAVEFISH,	/* 33 */
	CHIP_BEIGE_GOBY,	/* 34 */
	CHIP_YELLOW_CARP,	/* 35 */
	CHIP_IP_DISCOVERY,	/* 36 */
	CHIP_LAST,
};

extern const char *amdgpu_asic_name[];

struct amdgpu_asic_type_quirk {
	unsigned short device;	/* PCI device ID */
	u8 revision;	/* revision ID */
	unsigned short type;	/* real ASIC type */
};
#endif /*__AMD_ASIC_TYPE_H__ */
