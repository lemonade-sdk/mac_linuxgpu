/* linuxu: SHIM (vendor/linux/arch/arm64/include/vdso/bits.h)
 * Integer widths and literal helpers. Clock identifiers belong to the
 * platform time.h used by the host and DriverKit clock adapters.
 */
#ifndef __VDSO_BITS_H
#define __VDSO_BITS_H
#define __BITS_PER_LONG		64
#define __BITS_PER_LONG_LONG	64
#define _UL(x) ((unsigned long)(x))
#define _ULL(x) ((unsigned long long)(x))
#define _BIT128(nr)		(((unsigned __int128)1) << (nr))

#endif
