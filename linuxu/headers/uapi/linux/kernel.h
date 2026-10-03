/* linuxu: SHIM (third_party/linux/include/uapi/linux/kernel.h)
 *
 * The pinned tree's uapi PAGE_SIZE is 4096, but the linuxu mm model is
 * 16 KB pages (PAGE_SHIFT=14): the allocator
 * (linuxu/src/mm/page.c) backs every page slot with 16 KB and all KMD
 * PAGE_SIZE arithmetic must agree.  Shadow the constant so any TU that
 * only sees the uapi copy (via <linux/math.h>) matches mm.h. */
#ifndef _UAPI_LINUX_KERNEL_H
#define _UAPI_LINUX_KERNEL_H
#define PAGE_SIZE	(1UL << 14)
#endif
