/* A fixture DCN 4.0.1 display (dcn401_fixture.c), shared by the offline
 * display tests: the register file, the IP bases, a synthetic VBIOS with
 * three DisplayPort paths and one HDMI path, and synthetic EDIDs. */
#ifndef DCN401_FIXTURE_H
#define DCN401_FIXTURE_H
#include <stdint.h>

struct amdgpu_device;

#define DCN401_FIXTURE_PATHS 4u
extern uint32_t dcn401_dce_base[6];
extern unsigned long dcn401_reg_reads, dcn401_reg_writes;
/* EDID 1.4, 1920x1080@60 preferred: with an HDMI CTA block, and DP-only. */
extern uint8_t dcn401_fixture_edid[256];
extern uint8_t dcn401_fixture_edid_dp[128];

/* DCN 4.0.1 IP version and bases, the register file behind adev->reg.pcie
 * (every register reads back what was last written), the DMCUB fuse. */
void dcn401_fixture_attach(struct amdgpu_device *adev);
/* The synthetic VBIOS, through amdgpu_atombios_init. */
void dcn401_fixture_vbios(struct amdgpu_device *adev);
void dcn401_fixture_build_edid(void);
#endif
