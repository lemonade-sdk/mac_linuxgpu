/* The dext's string functions. Device memory takes the VRAM aperture's
 * accesses (below): BAR0 is the only device mapping the driver addresses
 * by pointer, every access through it is a call into the kernel, and other
 * MMIO (registers, doorbells) goes through ioremap tokens, never through
 * these functions. Every other pointer is normal, cacheable memory (DMA
 * buffers, TTM pages, kernel heap), copied here with 16-byte NEON loads and
 * stores. They are byte-element accesses (LD1/ST1 .16B), so they need no
 * alignment, which -mstrict-align and normal memory both allow, and nothing
 * here uses the cache-zeroing instruction (DC ZVA). no_builtin keeps the
 * compiler from turning these loops back into calls to themselves (in the
 * dext they are memcpy, memset and memmove). No locks, allocation or mapping
 * lookup here: callers may hold allocator/PCI locks. */
#include <rt/device_string.h>
#include <stdint.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define RAM_STRING __attribute__((no_builtin))

static void ram_copy_forward(unsigned char *d, const unsigned char *s, size_t n);

/* ---- the VRAM aperture (rt/device_string.h) ---- */
static uintptr_t aperture_lo, aperture_hi;
static int aperture_gone_flag;
static const struct linuxu_aperture_ops *aperture_ops;
static struct linuxu_aperture_stats aperture_counts;

void linuxu_aperture_set(uintptr_t base, uint64_t size)
{
	__atomic_store_n(&aperture_hi, base && size ? base + size : 0, __ATOMIC_RELEASE);
	__atomic_store_n(&aperture_lo, base, __ATOMIC_RELEASE);
	/* A new mapping belongs to a device that is present. */
	if (base)
		__atomic_store_n(&aperture_gone_flag, 0, __ATOMIC_RELEASE);
}

void linuxu_aperture_set_ops(const struct linuxu_aperture_ops *ops)
{
	__atomic_store_n(&aperture_ops, ops, __ATOMIC_RELEASE);
}

/* Weak: the driver logs through printk.c; host tests print nothing. */
__attribute__((weak)) void linuxu_aperture_report(const char *why) { (void)why; }

void linuxu_aperture_gone(const char *why)
{
	if (!__atomic_exchange_n(&aperture_gone_flag, 1, __ATOMIC_ACQ_REL))
		linuxu_aperture_report(why);
}

int linuxu_aperture_is_gone(void)
{
	return __atomic_load_n(&aperture_gone_flag, __ATOMIC_ACQUIRE);
}

void linuxu_aperture_stats(struct linuxu_aperture_stats *out)
{
	out->stores = __atomic_load_n(&aperture_counts.stores, __ATOMIC_RELAXED);
	out->loads = __atomic_load_n(&aperture_counts.loads, __ATOMIC_RELAXED);
	out->skipped = __atomic_load_n(&aperture_counts.skipped, __ATOMIC_RELAXED);
	out->calls = __atomic_load_n(&aperture_counts.calls, __ATOMIC_RELAXED);
}

int linuxu_aperture_contains(const volatile void *p, size_t size)
{
	const uintptr_t a = (uintptr_t)p;
	const uintptr_t lo = __atomic_load_n(&aperture_lo, __ATOMIC_RELAXED);

	return lo && size && a < __atomic_load_n(&aperture_hi, __ATOMIC_RELAXED) && a + size > lo;
}

static inline uint64_t aperture_offset(const volatile void *p)
{
	return (uint64_t)((uintptr_t)p - __atomic_load_n(&aperture_lo, __ATOMIC_RELAXED));
}

/* One access of @width bytes (aligned): through the ops, or plain memory
 * when the aperture has none (host tests). Once gone: skipped, ones. */
static void aperture_access(volatile void *p, void *value, unsigned int width, int store)
{
	const struct linuxu_aperture_ops *ops = __atomic_load_n(&aperture_ops, __ATOMIC_ACQUIRE);

	if (__atomic_load_n(&aperture_gone_flag, __ATOMIC_ACQUIRE)) {
		__atomic_add_fetch(&aperture_counts.skipped, 1, __ATOMIC_RELAXED);
		if (!store)
			for (unsigned int i = 0; i < width; i++) ((unsigned char *)value)[i] = 0xff;
		return;
	}
	if (ops) {
		__atomic_add_fetch(&aperture_counts.calls, 1, __ATOMIC_RELAXED);
		/* Earlier stores to normal memory (what the device will read)
		 * reach memory before the device is told, as dma_wmb before a
		 * BAR store; the call itself is the access. */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (store)
			ops->write(aperture_offset(p), value, width);
		else
			ops->read(aperture_offset(p), value, width);
		return;
	}
	switch (width) {
	case 1: if (store) *(volatile uint8_t *)p = *(uint8_t *)value; else *(uint8_t *)value = *(volatile uint8_t *)p; break;
	case 2: if (store) *(volatile uint16_t *)p = *(uint16_t *)value; else *(uint16_t *)value = *(volatile uint16_t *)p; break;
	case 4: if (store) *(volatile uint32_t *)p = *(uint32_t *)value; else *(uint32_t *)value = *(volatile uint32_t *)p; break;
	default: if (store) *(volatile uint64_t *)p = *(uint64_t *)value; else *(uint64_t *)value = *(volatile uint64_t *)p; break;
	}
}

void linuxu_aperture_read(const volatile void *p, void *value, unsigned int width)
{
	__atomic_add_fetch(&aperture_counts.loads, 1, __ATOMIC_RELAXED);
	aperture_access((volatile void *)p, value, width, 0);
}

void linuxu_aperture_write(volatile void *p, const void *value, unsigned int width)
{
	__atomic_add_fetch(&aperture_counts.stores, 1, __ATOMIC_RELAXED);
	aperture_access(p, (void *)value, width, 1);
}

/* The widest aligned access that fits at @address with @size left. */
static inline unsigned int aperture_width(uintptr_t address, size_t size)
{
	if (!(address & 7) && size >= 8) return 8;
	if (!(address & 3) && size >= 4) return 4;
	if (!(address & 1) && size >= 2) return 2;
	return 1;
}

void linuxu_aperture_copy_in(volatile void *destination, const void *source, size_t size)
{
	volatile unsigned char *d = destination;
	const unsigned char *s = source;

	__atomic_add_fetch(&aperture_counts.stores, 1, __ATOMIC_RELAXED);
	while (size) {
		const unsigned int w = aperture_width((uintptr_t)d, size);
		uint64_t v = 0;

		for (unsigned int i = 0; i < w; i++) ((unsigned char *)&v)[i] = s[i];
		aperture_access(d, &v, w, 1);
		d += w; s += w; size -= w;
	}
}

void linuxu_aperture_copy_out(void *destination, const volatile void *source, size_t size)
{
	unsigned char *d = destination;
	const volatile unsigned char *s = source;

	__atomic_add_fetch(&aperture_counts.loads, 1, __ATOMIC_RELAXED);
	while (size) {
		const unsigned int w = aperture_width((uintptr_t)s, size);
		uint64_t v = 0;

		aperture_access((volatile void *)s, &v, w, 0);
		for (unsigned int i = 0; i < w; i++) d[i] = ((unsigned char *)&v)[i];
		d += w; s += w; size -= w;
	}
}

void linuxu_aperture_fill(volatile void *destination, int value, size_t size)
{
	volatile unsigned char *d = destination;
	const uint64_t pattern = (uint64_t)(unsigned char)value * UINT64_C(0x0101010101010101);

	__atomic_add_fetch(&aperture_counts.stores, 1, __ATOMIC_RELAXED);
	while (size) {
		const unsigned int w = aperture_width((uintptr_t)d, size);
		uint64_t v = pattern;

		aperture_access(d, &v, w, 1);
		d += w; size -= w;
	}
}

/* Copies with an aperture end, through a bounce buffer when both are. */
static void aperture_copy(void *destination, const void *source, size_t size)
{
	const int to = linuxu_aperture_contains(destination, size);
	const int from = linuxu_aperture_contains(source, size);
	unsigned char bounce[256];

	if (!to && !from) {
		ram_copy_forward(destination, source, size);
	} else if (to && !from) {
		linuxu_aperture_copy_in(destination, source, size);
	} else if (from && !to) {
		linuxu_aperture_copy_out(destination, source, size);
	} else {
		for (size_t done = 0; done < size;) {
			const size_t n = size - done < sizeof(bounce) ? size - done : sizeof(bounce);

			linuxu_aperture_copy_out(bounce, (const unsigned char *)source + done, n);
			linuxu_aperture_copy_in((unsigned char *)destination + done, bounce, n);
			done += n;
		}
	}
}

#if defined(__aarch64__)
/* 16 bytes with LD1/ST1 (.16B): byte elements, so no alignment needed. As
 * target instructions they stay whole under -mstrict-align, which splits a
 * plain unaligned vector load into byte loads. */
static inline uint8x16_t ram_load16(const unsigned char *p)
{
	uint8x16_t v;

	__asm__("ld1 {%0.16b}, [%1]" : "=w"(v) : "r"(p) : "memory");
	return v;
}
static inline void ram_store16(unsigned char *p, uint8x16_t v)
{
	__asm__ volatile("st1 {%0.16b}, [%1]" : : "w"(v), "r"(p) : "memory");
}
#endif

/* Normal memory, forward: @d may overlap @s only below it (memmove). */
static RAM_STRING void ram_copy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
#if defined(__aarch64__)
	while (n >= 64) {
		const uint8x16x4_t v = vld1q_u8_x4(s);

		vst1q_u8_x4(d, v);
		d += 64; s += 64; n -= 64;
	}
	while (n >= 16) {
		ram_store16(d, ram_load16(s));
		d += 16; s += 16; n -= 16;
	}
#endif
	while (n--) *d++ = *s++;
}

/* Normal memory, backward: @d may overlap @s only above it (memmove). */
static RAM_STRING void ram_copy_backward(unsigned char *d, const unsigned char *s, size_t n)
{
	d += n; s += n;
#if defined(__aarch64__)
	while (n >= 64) {
		d -= 64; s -= 64; n -= 64;
		const uint8x16x4_t v = vld1q_u8_x4(s);

		vst1q_u8_x4(d, v);
	}
	while (n >= 16) {
		d -= 16; s -= 16; n -= 16;
		ram_store16(d, ram_load16(s));
	}
#endif
	while (n--) *--d = *--s;
}

RAM_STRING void *linuxu_device_memset(void *destination, int value, size_t size)
{
	if (linuxu_aperture_contains(destination, size)) {
		linuxu_aperture_fill(destination, value, size);
		return destination;
	}
	unsigned char *d = destination;
	const unsigned char byte = (unsigned char)value;
#if defined(__aarch64__)
	const uint8x16_t v = vdupq_n_u8(byte);
	const uint8x16x4_t v4 = {{v, v, v, v}};

	while (size >= 64) {
		vst1q_u8_x4(d, v4);
		d += 64; size -= 64;
	}
	while (size >= 16) {
		ram_store16(d, v);
		d += 16; size -= 16;
	}
#endif
	while (size--) *d++ = byte;
	return destination;
}

RAM_STRING void *linuxu_device_memcpy(void *destination, const void *source, size_t size)
{
	if (linuxu_aperture_contains(destination, size) || linuxu_aperture_contains(source, size)) {
		aperture_copy(destination, source, size);
		return destination;
	}
	ram_copy_forward(destination, source, size);
	return destination;
}

RAM_STRING void *linuxu_device_memmove(void *destination, const void *source, size_t size)
{
	if (!size || destination == source) return destination;
	if (linuxu_aperture_contains(destination, size) || linuxu_aperture_contains(source, size)) {
		/* Overlapping moves within the aperture: chunks in the safe order. */
		unsigned char bounce[256];
		const int backward = (uintptr_t)destination > (uintptr_t)source;

		for (size_t done = 0; done < size;) {
			const size_t n = size - done < sizeof(bounce) ? size - done : sizeof(bounce);
			const size_t at = backward ? size - done - n : done;

			aperture_copy(bounce, (const unsigned char *)source + at, n);
			aperture_copy((unsigned char *)destination + at, bounce, n);
			done += n;
		}
		return destination;
	}
	if ((uintptr_t)destination < (uintptr_t)source ||
	    (uintptr_t)destination - (uintptr_t)source >= size)
		ram_copy_forward(destination, source, size);
	else
		ram_copy_backward(destination, source, size);
	return destination;
}

RAM_STRING int linuxu_device_memcmp(const void *first, const void *second, size_t size)
{
	if (linuxu_aperture_contains(first, size) || linuxu_aperture_contains(second, size)) {
		unsigned char x[128], y[128];

		if (linuxu_aperture_is_gone())
			return 1;	/* nothing to compare on a vanished device */
		for (size_t done = 0; done < size;) {
			const size_t n = size - done < sizeof(x) ? size - done : sizeof(x);

			aperture_copy(x, (const unsigned char *)first + done, n);
			aperture_copy(y, (const unsigned char *)second + done, n);
			for (size_t i = 0; i < n; i++)
				if (x[i] != y[i]) return (int)x[i] - (int)y[i];
			done += n;
		}
		return 0;
	}
	const unsigned char *a = first, *b = second;
#if defined(__aarch64__)
	/* Equal 16-byte blocks skipped whole; the first that differs is compared
	 * byte by byte below. */
	while (size >= 16 && vminvq_u8(vceqq_u8(ram_load16(a), ram_load16(b))) == 0xff) {
		a += 16; b += 16; size -= 16;
	}
#endif
	while (size--) {
		const unsigned char av = *a++, bv = *b++;
		if (av != bv) return (int)av - (int)bv;
	}
	return 0;
}

void linuxu_device_bzero(void *destination, size_t size)
{
	(void)linuxu_device_memset(destination, 0, size);
}

#ifdef LINUXU_DEXT_DK
/* Compiler-generated aggregate copies/clears can emit plain C ABI calls,
 * independently of source aliases. Resolve those inside the dext as well. */
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef bzero
void *memset(void *d, int c, size_t n) { return linuxu_device_memset(d, c, n); }
void *memcpy(void *d, const void *s, size_t n) { return linuxu_device_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return linuxu_device_memmove(d, s, n); }
int memcmp(const void *a, const void *b, size_t n) { return linuxu_device_memcmp(a, b, n); }
void bzero(void *d, size_t n) { linuxu_device_bzero(d, n); }
#endif
