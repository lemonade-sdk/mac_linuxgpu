/* Scalar, aligned accesses for kernel buffers, including uncached PCI VRAM.
 * Volatile prevents loop recognition from reintroducing libc, vector accesses
 * or cache-zero instructions. No locks, allocation or mapping lookup here:
 * callers may hold allocator/PCI locks. Mapping ownership remains the caller's.
 * This deliberately uses the same access contract for RAM and device memory. */
#include <rt/device_string.h>
#include <stdint.h>

typedef uint64_t device_word __attribute__((__may_alias__));

void *linuxu_device_memset(void *destination, int value, size_t size)
{
	volatile unsigned char *d = destination;
	unsigned char byte = (unsigned char)value;
	device_word word = (device_word)byte * UINT64_C(0x0101010101010101);
	while (size && ((uintptr_t)d & 7)) {
		*d++ = byte;
		--size;
	}
	while (size >= sizeof(word)) {
		*(volatile device_word *)d = word;
		d += sizeof(word);
		size -= sizeof(word);
	}
	while (size--) *d++ = byte;
	return destination;
}

void *linuxu_device_memcpy(void *destination, const void *source, size_t size)
{
	volatile unsigned char *d = destination;
	const volatile unsigned char *s = source;
	/* If the alignments differ, byte accesses avoid unaligned device loads. */
	if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
		while (size && ((uintptr_t)d & 7)) {
			*d++ = *s++;
			--size;
		}
		while (size >= sizeof(device_word)) {
			*(volatile device_word *)d = *(const volatile device_word *)s;
			d += sizeof(device_word);
			s += sizeof(device_word);
			size -= sizeof(device_word);
		}
	}
	while (size--) *d++ = *s++;
	return destination;
}

void *linuxu_device_memmove(void *destination, const void *source, size_t size)
{
	if (!size || destination == source) return destination;
	if ((uintptr_t)destination < (uintptr_t)source ||
	    (uintptr_t)destination - (uintptr_t)source >= size)
		return linuxu_device_memcpy(destination, source, size);
	volatile unsigned char *d = (volatile unsigned char *)destination + size;
	const volatile unsigned char *s = (const volatile unsigned char *)source + size;
	if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
		while (size && ((uintptr_t)d & 7)) {
			*--d = *--s;
			--size;
		}
		while (size >= sizeof(device_word)) {
			d -= sizeof(device_word);
			s -= sizeof(device_word);
			*(volatile device_word *)d = *(const volatile device_word *)s;
			size -= sizeof(device_word);
		}
	}
	while (size--) *--d = *--s;
	return destination;
}

int linuxu_device_memcmp(const void *first, const void *second, size_t size)
{
	const volatile unsigned char *a = first, *b = second;
	while (size--) {
		unsigned char av = *a++, bv = *b++;
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
