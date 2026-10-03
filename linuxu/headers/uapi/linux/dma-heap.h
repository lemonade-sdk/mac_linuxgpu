/* linuxu: SHIM (third_party/linux/include/uapi/linux/dma-heap.h) */
#ifndef _UAPI_LINUX_DMA_HEAP_H
#define _UAPI_LINUX_DMA_HEAP_H

#include <linux/types.h>

#ifndef _IOWR
#include <sys/ioccom.h>
#endif

#define DMA_HEAP_IOCTL_ALLOC	_IOWR('w', 0x0, struct dma_heap_allocation_data)

struct dma_heap_allocation_data {
	__u64 fd;
	__u64 len;
	__u64 align;
	__u32 fd_flags;
	__u32 heap_flags;
	__u64 handle;
	__s64 base_addr;
};

#endif /* _UAPI_LINUX_DMA_HEAP_H */
