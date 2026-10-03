/* Kernel buffers may be uncached PCI mappings. Darwin's libc string routines
 * assume normal memory (large bzero uses DC ZVA), unlike these mappings.
 * Include before SDK string macros in every DriverKit KMD translation unit. */
#ifndef LINUXU_DEVICE_STRING_H
#define LINUXU_DEVICE_STRING_H
#include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void *linuxu_device_memset(void *, int, size_t);
void *linuxu_device_memcpy(void *, const void *, size_t);
void *linuxu_device_memmove(void *, const void *, size_t);
int linuxu_device_memcmp(const void *, const void *, size_t);
void linuxu_device_bzero(void *, size_t);
#ifdef __cplusplus
}
#endif

/* Object-like aliases also cover function pointers. Explicit builtins occur
 * in upstream SMU headers; they must obey the same device-memory contract. */
#ifdef LINUXU_DEXT_DK
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
