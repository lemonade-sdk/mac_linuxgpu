#ifndef LINUXU_RT_GART_H
#define LINUXU_RT_GART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct amdgpu_device;
struct amdgpu_gmc;
#ifndef __cplusplus
enum amdgpu_gart_placement;
#endif

/* Set an immutable per-start host VA / GPU GART address before PCI probe.
 * The caller reserves the matching aligned Mach VA first. The actual FB
 * overlap check runs when GMC discovers the GPU's memory layout. */
int rt_gart_set_window(uint64_t base);
/* Returns the requested/selected base and fixed aperture size. Before a
 * request, base is zero. A rejected placement returns its error. */
int rt_gart_get_window(uint64_t *base, uint64_t *size);
/* 0 only once the upstream GMC placement hook accepted the actual FB layout;
 * -EAGAIN before selection, or a recorded placement error. */
int rt_gart_status(void);
/* Only call after GPU work, BOs, GART mappings and PCI are quiesced. */
void rt_gart_reset(void);

/* Build glue renames the call site in gmc_v12_0.c to this wrapper. */
#ifndef __cplusplus
void rt_amdgpu_gmc_gart_location(struct amdgpu_device *adev,
				struct amdgpu_gmc *mc,
				enum amdgpu_gart_placement placement);
#endif

#ifdef __cplusplus
}
#endif
#endif
