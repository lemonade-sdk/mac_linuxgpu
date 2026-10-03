/* linuxu: SHIM (third_party/linux/include/asm-generic/agp.h) */
#ifndef _ASM_AGP_H
#define _ASM_AGP_H

#include <linux/io.h>

#define map_page_into_agp(page) do {} while (0)
#define unmap_page_from_agp(page) do {} while (0)
#define flush_agp_cache() mb()

#endif /* _ASM_AGP_H */
