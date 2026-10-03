/* linuxu: SHIM (third_party/linux/include/linux/gfp.h)
 *
 * GFP flag definitions + minimal allocation-context API. The flags
 * are plain bits (upstream uses bitwise-annotated ints; the shim uses
 * plain u32). The few functions in the closure are declared extern.
 */
#ifndef _LINUX_GFP_H
#define _LINUX_GFP_H

#include <linux/types.h>
#include <linux/numa.h>

/* ---- gfp_t flag bits (upstream values) ---- */
#define __GFP_DMA32		0x00000001U
#define __GFP_MOVABLE		0x00000002U
#define __GFP_WAIT		0x00000010U
#define __GFP_HIGH		0x00000020U
#define __GFP_COLD		0x00000040U
#define __GFP_ZERO		0x00000080U
#define __GFP_ATOMIC		0x00000100U
#define __GFP_DIRECT_RECLAIM	0x00000200U
#define __GFP_KSWAPD_RECLAIM	0x00000400U
#define __GFP_MEMALLOC		0x00000800U
#define __GFP_NOFAIL		0x00001000U
#define __GFP_NORETRY		0x00002000U
#define __GFP_COMP		0x00004000U
#define __GFP_NOMEMALLOC	0x00008000U
#define __GFP_HARDWALL		0x00010000U
#define __GFP_RECLAIMABLE	0x00020000U
#define __GFP_NOTRACK		0x00040000U
#define __GFP_ACCOUNT		0x00100000U
#define __GFP_IO		0x01000000U
#define __GFP_FS		0x02000000U
#define __GFP_NOWARN		0x04000000U
#define __GFP_THISNODE		0x08000000U
#define __GFP_RETRY_MAYFAIL	0x10000000U
#define __GFP_NOTRACK		0x00040000U
#define __GFP_IO_FS		(__GFP_IO | __GFP_FS)
#define __GFP_RECLAIM		(__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
#define __GFP_RECLAIM_MASK	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define __GFP_ALLOC_MASK	(__GFP_HIGH | __GFP_ATOMIC)
#define __GFP_OTHER_MASK	(__GFP_HARDWALL | __GFP_ACCOUNT | __GFP_NOTRACK)
#define __GFP_BITS_SHIFT	(8 * (int)sizeof(unsigned int))

#define GFP_DMA		0U
#define GFP_HIGHMEM	0U
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_ATOMIC	(__GFP_ATOMIC | __GFP_HARDWALL)
#define GFP_NOWAIT	(__GFP_ATOMIC | __GFP_HARDWALL | __GFP_NORETRY)
#define GFP_USER	(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_HIGHUSER	(GFP_USER)
#define GFP_HIGHUSER_MOVABLE (GFP_USER | __GFP_MOVABLE)
#define GFP_DMA32	(__GFP_DMA32)
#define GFP_NOFS	(__GFP_RECLAIM | __GFP_IO)
#define GFP_NOIO	(__GFP_RECLAIM)

/* ---- allocation context (mostly no-op in userspace) ---- */
struct alloc_tag;

static inline void gfp_get_tag(gfp_t *flags, struct alloc_tag *tag) { }
static inline struct alloc_tag *gfp_to_tag(gfp_t flags) { return NULL; }

/* ---- runtime (linuxu/src/kmem/) ---- */
static inline bool gfpflags_allow_blocking(gfp_t flags)
{
	return !!(flags & __GFP_DIRECT_RECLAIM);
}
extern int  cpuset_current_mems_allowed(void);

#endif /* _LINUX_GFP_H */
