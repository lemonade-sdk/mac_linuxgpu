/* linuxu: SHIM (third_party/linux/include/linux/iosys-map.h)
 *
 * Aligned to the 2026 pinned layout: struct iosys_map is a tagged union of a
 * system-memory pointer (vaddr) and an I/O-memory pointer (vaddr_iomem) plus
 * the is_iomem flag. In the userspace dext there is no real WC I/O memory, so
 * "iomem" addresses are just ordinary pointers; memcpy_toio/fromio reduce to
 * memcpy. The accessors mirror the vendor inlines verbatim (minus the
 * CONFIG-variant read/write word helpers that ttm/drm don't call here).
 */
#ifndef _LINUX_IOSYS_MAP_H
#define _LINUX_IOSYS_MAP_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/io.h>

#ifndef __iomem
#define __iomem
#endif

struct iosys_map {
	union {
		void __iomem *vaddr_iomem;
		void *vaddr;
	};
	bool is_iomem;
};

#define IOSYS_MAP_INIT_VADDR(vaddr_)		\
{						\
	.vaddr = (vaddr_),			\
	.is_iomem = false,			\
}

#define IOSYS_MAP_INIT_VADDR_IOMEM(vaddr_iomem_)	\
{						\
	.vaddr_iomem = (vaddr_iomem_),		\
	.is_iomem = true,			\
}

#define IOSYS_MAP_INIT_OFFSET(map_, offset_) ({		\
	struct iosys_map copy_ = *(map_);			\
	iosys_map_incr(&copy_, (offset_));		\
	copy_;						\
})

static inline void iosys_map_set_vaddr(struct iosys_map *map, void *vaddr)
{
	map->vaddr = vaddr;
	map->is_iomem = false;
}

static inline void iosys_map_set_vaddr_iomem(struct iosys_map *map,
					     void __iomem *vaddr_iomem)
{
	map->vaddr_iomem = vaddr_iomem;
	map->is_iomem = true;
}

static inline bool iosys_map_is_equal(const struct iosys_map *lhs,
				      const struct iosys_map *rhs)
{
	if (lhs->is_iomem != rhs->is_iomem)
		return false;
	else if (lhs->is_iomem)
		return lhs->vaddr_iomem == rhs->vaddr_iomem;
	else
		return lhs->vaddr == rhs->vaddr;
}

static inline bool iosys_map_is_null(const struct iosys_map *map)
{
	if (map->is_iomem)
		return !map->vaddr_iomem;
	return !map->vaddr;
}

static inline bool iosys_map_is_set(const struct iosys_map *map)
{
	return !iosys_map_is_null(map);
}

static inline void iosys_map_clear(struct iosys_map *map)
{
	memset(map, 0, sizeof(*map));
}

static inline void iosys_map_memcpy_to(struct iosys_map *dst, size_t dst_offset,
				       const void *src, size_t len)
{
	if (dst->is_iomem)
		memcpy_toio(dst->vaddr_iomem + dst_offset, src, len);
	else
		memcpy(dst->vaddr + dst_offset, src, len);
}

static inline void iosys_map_memcpy_from(void *dst, const struct iosys_map *src,
					 size_t src_offset, size_t len)
{
	if (src->is_iomem)
		memcpy_fromio(dst, src->vaddr_iomem + src_offset, len);
	else
		memcpy(dst, src->vaddr + src_offset, len);
}

static inline void iosys_map_incr(struct iosys_map *map, size_t incr)
{
	if (map->is_iomem)
		map->vaddr_iomem += incr;
	else
		map->vaddr += incr;
}

static inline void iosys_map_memset(struct iosys_map *dst, size_t offset,
				    u8 value, size_t len)
{
	if (dst->is_iomem)
		memset_io(dst->vaddr_iomem + offset, value, len);
	else
		memset(dst->vaddr + offset, value, len);
}

/* ---- legacy accessors kept for older shim code (map to the 2026 union) ---- */
static inline bool iosys_map_is_valid(const struct iosys_map *map)
{
	return iosys_map_is_set(map);
}
static inline bool iosys_map_is_virt(const struct iosys_map *map)
{
	return !map->is_iomem;
}
static inline void *iosys_map_get_virt(const struct iosys_map *map)
{
	return map->vaddr;
}
static inline void iosys_map_set_virt(struct iosys_map *map, void __iomem *addr)
{
	map->vaddr = (void *)addr;
	map->is_iomem = false;
}
static inline void iosys_map_set_virt_noinc(struct iosys_map *map, void __iomem *addr)
{
	iosys_map_set_virt(map, addr);
}
static inline u8  iosys_map_read_u8 (const struct iosys_map *map, size_t i) { return (u8)  *(unsigned char *)(map->vaddr + i); }
static inline u16 iosys_map_read_u16(const struct iosys_map *map, size_t i) { return (u16) *(unsigned short *)(map->vaddr + i); }
static inline u32 iosys_map_read_u32(const struct iosys_map *map, size_t i) { return (u32) *(unsigned int  *)(map->vaddr + i); }
static inline u64 iosys_map_read_u64(const struct iosys_map *map, size_t i) { return (u64) *(unsigned long long *)(map->vaddr + i); }
static inline void iosys_map_write_u8 (struct iosys_map *map, size_t i, u8  v) { *(unsigned char *)      (map->vaddr + i) = v; }
static inline void iosys_map_write_u16(struct iosys_map *map, size_t i, u16 v) { *(unsigned short *)     (map->vaddr + i) = v; }
static inline void iosys_map_write_u32(struct iosys_map *map, size_t i, u32 v) { *(unsigned int *)       (map->vaddr + i) = v; }
static inline void iosys_map_write_u64(struct iosys_map *map, size_t i, u64 v) { *(unsigned long long *) (map->vaddr + i) = v; }

#endif /* _LINUX_IOSYS_MAP_H */
