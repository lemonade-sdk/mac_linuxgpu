/* linuxu: SHIM (third_party/linux/include/linux/dma-addr.h) */
#ifndef _LINUX_DMA_ADDR_H
#define _LINUX_DMA_ADDR_H

#include <linux/types.h>

#define DMA_ADDR_INVALID ((dma_addr_t)~0)
#define dma_addr_is_valid(addr)		((addr) != DMA_ADDR_INVALID)
#define dma_pfn(addr)		(PFN_DOWN((addr)))
#define pfn_dma(pfn)		(PFN_PHYS((pfn)))
#define DMA_BIT_MASK(n)		(((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_NO_DMA_ADDR		DMA_ADDR_INVALID
#define dma_addr_is_valid(addr)		((addr) != DMA_ADDR_INVALID)

#endif /* _LINUX_DMA_ADDR_H */
