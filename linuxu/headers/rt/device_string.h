/* Kernel buffers may be uncached PCI mappings. Darwin's libc string routines
 * assume normal memory (large bzero uses DC ZVA), unlike these mappings.
 * Include before SDK string macros in every DriverKit KMD translation unit. */
#ifndef LINUXU_DEVICE_STRING_H
#define LINUXU_DEVICE_STRING_H
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void *linuxu_device_memset(void *, int, size_t);
void *linuxu_device_memcpy(void *, const void *, size_t);
void *linuxu_device_memmove(void *, const void *, size_t);
int linuxu_device_memcmp(const void *, const void *, size_t);
void linuxu_device_bzero(void *, size_t);

/* The VRAM aperture the driver maps into its own address space
 * (dext_bar0_cpu_map). A CPU store or load through that mapping to a
 * device that has left the bus (a Thunderbolt power-off) is not an error
 * the process can see: on Apple silicon it is an uncorrectable bus error
 * that panics the Mac. Linux reaches the same state through
 * amdgpu_device_unmap_mmio after a hot unplug; here the mapping is gated:
 * once the device is known gone (linuxu_aperture_gone), the string
 * functions skip stores into the aperture and read all ones from it.
 * Accesses through kernel IOPCIDevice calls (registers, doorbells) are
 * refused by the kernel instead and need no gate. */
void linuxu_aperture_set(uintptr_t base, uint64_t size);
void linuxu_aperture_gone(const char *why);
int linuxu_aperture_is_gone(void);
struct linuxu_aperture_stats {
	unsigned long long stores, loads;	/* string operations through it */
	unsigned long long skipped;		/* refused once gone */
};
void linuxu_aperture_stats(struct linuxu_aperture_stats *out);
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
