/* The VRAM aperture's accesses (declarations only; device_string.c). Kept
 * apart from rt/device_string.h, which also aliases the string functions,
 * so linux/io.h can route through it without the aliases. */
#ifndef LINUXU_RT_APERTURE_H
#define LINUXU_RT_APERTURE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* The VRAM aperture: the address range kernel VRAM mappings (amdgpu's
 * aper_base_kaddr, TTM kmaps) point into. A CPU store or load through a
 * user-space mapping of the BAR to a device that has left the bus (a
 * Thunderbolt unplug) is not an error the process can see: on Apple
 * silicon it is an uncorrectable bus error that panics the Mac, and it is
 * reported only after the store retired, so no check before the store can
 * prevent it (build 240's panic: the DMUB inbox push, with the removal
 * already under way). So no access goes through such a mapping: every
 * access to the aperture goes through @ops (the dext: the kernel's
 * IOPCIDevice MemoryRead/MemoryWrite on BAR0, which register accesses
 * have always used, through unplugs, without a fault). The string
 * functions below and linux/io.h's accessors route through it; a direct
 * dereference of an aperture pointer is a bug (the CS fixture backs the
 * aperture with no-access memory so the offline tests find any).
 *
 * Without @ops the aperture is plain memory (host tests of the gate).
 * Once the device is known gone (linuxu_aperture_gone), writes are
 * skipped and reads return all ones, without a kernel call. */
struct linuxu_aperture_ops {
	/* @width (1, 2, 4 or 8, aligned) bytes at byte @offset into the aperture. */
	void (*read)(uint64_t offset, void *value, unsigned int width);
	void (*write)(uint64_t offset, const void *value, unsigned int width);
};
void linuxu_aperture_set(uintptr_t base, uint64_t size);
void linuxu_aperture_set_ops(const struct linuxu_aperture_ops *ops);
void linuxu_aperture_gone(const char *why);
int linuxu_aperture_is_gone(void);
int linuxu_aperture_contains(const volatile void *address, size_t size);
void linuxu_aperture_read(const volatile void *address, void *value, unsigned int width);
void linuxu_aperture_write(volatile void *address, const void *value, unsigned int width);
void linuxu_aperture_copy_in(volatile void *destination, const void *source, size_t size);
void linuxu_aperture_copy_out(void *destination, const volatile void *source, size_t size);
void linuxu_aperture_fill(volatile void *destination, int value, size_t size);
struct linuxu_aperture_stats {
	unsigned long long stores, loads;	/* operations into and out of it */
	unsigned long long skipped;		/* refused once gone */
	unsigned long long calls;		/* accesses through the ops */
};
void linuxu_aperture_stats(struct linuxu_aperture_stats *out);
#ifdef __cplusplus
}
#endif
#endif
