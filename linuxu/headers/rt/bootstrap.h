/* Explicit lifecycle for the linked Linux DRM and AMDGPU modules. */
#ifndef LINUXU_RT_BOOTSTRAP_H
#define LINUXU_RT_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call only after the Linux shim runtime is ready. No constructors run this. */
int linuxu_driver_bootstrap(void);
void linuxu_driver_shutdown(void);

/* Display (upstream Display Core / amdgpu_dm) is compiled in but off by
 * default: bootstrap sets the amdgpu.dc module parameter to 0, so amdgpu
 * adds no display IP block and probe is the compute-only probe. Passing a
 * non-zero value before linuxu_driver_bootstrap() restores upstream's
 * amdgpu.dc=-1 (DC on every ASIC that has it). Returns 0, or -EBUSY while
 * the modules are starting or running. */
int linuxu_driver_set_display(int enable);
int linuxu_driver_display_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
