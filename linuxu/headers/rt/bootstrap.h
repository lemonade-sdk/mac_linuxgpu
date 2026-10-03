/* Explicit lifecycle for the linked Linux DRM and AMDGPU modules. */
#ifndef LINUXU_RT_BOOTSTRAP_H
#define LINUXU_RT_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call only after the Linux shim runtime is ready. No constructors run this. */
int linuxu_driver_bootstrap(void);
void linuxu_driver_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
