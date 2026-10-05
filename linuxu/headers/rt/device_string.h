/* Kernel buffers may be uncached PCI mappings. Darwin's libc string routines
 * assume normal memory (large bzero uses DC ZVA), unlike these mappings.
 * Include before SDK string macros in every DriverKit KMD translation unit. */
#ifndef LINUXU_DEVICE_STRING_H
#define LINUXU_DEVICE_STRING_H
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <rt/aperture.h>

#ifdef __cplusplus
extern "C" {
#endif
void *linuxu_device_memset(void *, int, size_t);
void *linuxu_device_memcpy(void *, const void *, size_t);
void *linuxu_device_memmove(void *, const void *, size_t);
int linuxu_device_memcmp(const void *, const void *, size_t);
void linuxu_device_bzero(void *, size_t);

/* The VRAM aperture: rt/aperture.h. */
#ifdef __cplusplus
}
#endif

/* Object-like aliases also cover function pointers. Explicit builtins occur
 * in upstream SMU headers; they must obey the same device-memory contract.
 * Host builds of the driver alias them too (LINUXU_DEVICE_STRING_ALIASES),
 * so the offline tests route aperture accesses the way the dext does. */
#if defined(LINUXU_DEXT_DK) || defined(LINUXU_DEVICE_STRING_ALIASES)
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef bzero
#define memset linuxu_device_memset
#define memcpy linuxu_device_memcpy
#define memmove linuxu_device_memmove
#define memcmp linuxu_device_memcmp
#define bzero linuxu_device_bzero
#define __builtin_memset linuxu_device_memset
#define __builtin_memcpy linuxu_device_memcpy
#define __builtin_memmove linuxu_device_memmove
#define __builtin_memcmp linuxu_device_memcmp
#endif
#endif
