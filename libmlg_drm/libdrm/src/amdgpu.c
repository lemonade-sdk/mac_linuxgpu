/*
 * libdrm_amdgpu for mac_linuxgpu (amdgpu.h): devices, buffers and GPU
 * virtual address ranges, over the libdrm of this directory.
 *
 * Derived from libdrm's amdgpu_device.c, amdgpu_bo.c and amdgpu_vamgr.c,
 * reduced to what Mesa's ac_linux_drm layer uses, under their license:
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Differences from libdrm_amdgpu: the device keeps a dup(2) of the caller's
 * descriptor as libdrm does, and every device of this library is the same
 * GPU, so initialization deduplicates any two descriptors (libdrm compares
 * primary node names, which a render-node-only device does not have, and
 * then also treats the descriptors as equal). A dma-buf's size comes from
 * AMDGPU_GEM_OP rather than lseek(2), which the descriptor (a socket) does
 * not support. User memory buffers are not available. The marketing name
 * table holds only the RDNA4 boards. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xf86drm.h"
#include "amdgpu_drm.h"
#include "amdgpu.h"
#include "drm_internal.h"

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN(v, a) (((v) + (a) - 1) & ~((uint64_t)(a) - 1))
#define AMDGPU_INVALID_VA_ADDRESS 0xffffffffffffffffull
#define AMDGPU_VA_MGR_RESERVE_HALF_VA_FOR_PRT 0x1

/* ---- VA manager (amdgpu_vamgr.c) ---- */

struct va_hole {
	struct va_hole *prev, *next;	/* sorted by offset, highest first */
	uint64_t offset, size;
};

struct va_mgr {
	struct va_hole head;		/* list sentinel */
	uint64_t va_max, va_alignment;
	pthread_mutex_t mutex;
};

struct amdgpu_va {
	uint64_t address, size;
	struct va_mgr *vamgr;
};

static void hole_insert_after(struct va_hole *at, struct va_hole *n)
{
	n->prev = at;
	n->next = at->next;
	at->next->prev = n;
	at->next = n;
}

static void hole_remove(struct va_hole *h)
{
	h->prev->next = h->next;
	h->next->prev = h->prev;
	free(h);
}

static void vamgr_init(struct va_mgr *mgr, uint64_t start, uint64_t max, uint64_t alignment)
{
	struct va_hole *n = calloc(1, sizeof(*n));

	mgr->head.prev = mgr->head.next = &mgr->head;
	mgr->va_max = max;
	mgr->va_alignment = alignment;
	pthread_mutex_init(&mgr->mutex, NULL);
	if (n && max > start) {
		n->offset = start;
		n->size = max - start;
		hole_insert_after(&mgr->head, n);
	} else {
		free(n);
	}
}

static void vamgr_deinit(struct va_mgr *mgr)
{
	while (mgr->head.next != &mgr->head)
		hole_remove(mgr->head.next);
	pthread_mutex_destroy(&mgr->mutex);
}

static int vamgr_subtract_hole(struct va_hole *hole, uint64_t start_va, uint64_t end_va)
{
	if (start_va > hole->offset && end_va - hole->offset < hole->size) {
		struct va_hole *n = calloc(1, sizeof(*n));

		if (!n)
			return -ENOMEM;
		n->size = start_va - hole->offset;
		n->offset = hole->offset;
		hole_insert_after(hole, n);
		hole->size -= end_va - hole->offset;
		hole->offset = end_va;
	} else if (start_va > hole->offset) {
		hole->size = start_va - hole->offset;
	} else if (end_va - hole->offset < hole->size) {
		hole->size -= end_va - hole->offset;
		hole->offset = end_va;
	} else {
		hole_remove(hole);
	}
	return 0;
}

static int vamgr_find_va(struct va_mgr *mgr, uint64_t size, uint64_t alignment,
			 uint64_t base_required, bool search_from_top, uint64_t *va_out)
{
	struct va_hole *hole;
	uint64_t offset = 0;
	int r;

	alignment = MAX2(alignment, mgr->va_alignment);
	size = ALIGN(size, mgr->va_alignment);
	if (base_required % alignment)
		return -EINVAL;

	pthread_mutex_lock(&mgr->mutex);
	/* Bottom up walks the list from its tail (lowest offset first). */
	for (hole = search_from_top ? mgr->head.next : mgr->head.prev; hole != &mgr->head;
	     hole = search_from_top ? hole->next : hole->prev) {
		if (base_required) {
			if (hole->offset > base_required ||
			    hole->offset + hole->size < base_required + size)
				continue;
			offset = base_required;
		} else if (!search_from_top) {
			uint64_t waste = hole->offset % alignment;

			waste = waste ? alignment - waste : 0;
			offset = hole->offset + waste;
			if (offset >= hole->offset + hole->size ||
			    size > hole->offset + hole->size - offset)
				continue;
		} else {
			if (size > hole->size)
				continue;
			offset = hole->offset + hole->size - size;
			offset -= offset % alignment;
			if (offset < hole->offset)
				continue;
		}
		r = vamgr_subtract_hole(hole, offset, offset + size);
		pthread_mutex_unlock(&mgr->mutex);
		*va_out = offset;
		return r;
	}
	pthread_mutex_unlock(&mgr->mutex);
	return -ENOMEM;
}

static void vamgr_free_va(struct va_mgr *mgr, uint64_t va, uint64_t size)
{
	struct va_hole *hole, *next;

	if (va == AMDGPU_INVALID_VA_ADDRESS)
		return;
	size = ALIGN(size, mgr->va_alignment);

	pthread_mutex_lock(&mgr->mutex);
	/* hole: the lowest hole above @va (or the sentinel); next: the
	 * highest hole below it (or the sentinel). */
	hole = &mgr->head;
	for (next = mgr->head.next; next != &mgr->head; next = next->next) {
		if (next->offset < va)
			break;
		hole = next;
	}
	if (hole != &mgr->head && hole->offset == va + size) {
		/* Grow the upper hole, then merge the lower one. */
		hole->offset = va;
		hole->size += size;
		if (next != &mgr->head && next->offset + next->size == va) {
			next->size += hole->size;
			hole_remove(hole);
		}
		goto out;
	}
	if (next != &mgr->head && next->offset + next->size == va) {
		next->size += size;
		goto out;
	}
	next = calloc(1, sizeof(*next));
	if (next) {
		next->size = size;
		next->offset = va;
		hole_insert_after(hole, next);
	}
out:
	pthread_mutex_unlock(&mgr->mutex);
}

/* ---- devices (amdgpu_device.c) ---- */

struct amdgpu_bo {
	int refcount;
	struct amdgpu_device *dev;
	uint64_t alloc_size;
	uint32_t handle;
	pthread_mutex_t cpu_access_mutex;
	void *cpu_ptr;
	int cpu_map_count;
};

struct amdgpu_device {
	int refcount;
	struct amdgpu_device *next;
	int fd;
	unsigned major_version, minor_version;
	struct drm_amdgpu_info_device dev_info;
	struct va_mgr vamgr_32, vamgr_low, vamgr_high_32, vamgr_high;
	uint32_t address_prt_wa_control_bit;
	pthread_mutex_t bo_table_mutex;
	struct amdgpu_bo **bos;		/* indexed by GEM handle */
	uint32_t bos_cap;
};

static pthread_mutex_t dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct amdgpu_device *dev_list;

static int query_info(struct amdgpu_device *dev, unsigned id, unsigned size, void *value)
{
	struct drm_amdgpu_info request;

	memset(&request, 0, sizeof(request));
	request.return_pointer = (uintptr_t)value;
	request.return_size = size;
	request.query = id;
	return drmCommandWrite(dev->fd, DRM_AMDGPU_INFO, &request, sizeof(request));
}

static bool needs_smem_prt_wa(struct amdgpu_device *dev)
{
	struct drm_amdgpu_info_hw_ip ip;
	struct drm_amdgpu_info request;
	uint32_t count = 0, version, major, minor, rev;

	memset(&request, 0, sizeof(request));
	request.return_pointer = (uintptr_t)&count;
	request.return_size = sizeof(count);
	request.query = AMDGPU_INFO_HW_IP_COUNT;
	request.query_hw_ip.type = AMDGPU_HW_IP_GFX;
	if (drmCommandWrite(dev->fd, DRM_AMDGPU_INFO, &request, sizeof(request)) || !count)
		return false;
	memset(&ip, 0, sizeof(ip));
	memset(&request, 0, sizeof(request));
	request.return_pointer = (uintptr_t)&ip;
	request.return_size = sizeof(ip);
	request.query = AMDGPU_INFO_HW_IP_INFO;
	request.query_hw_ip.type = AMDGPU_HW_IP_GFX;
	if (drmCommandWrite(dev->fd, DRM_AMDGPU_INFO, &request, sizeof(request)))
		return false;
	if (dev->minor_version >= 48 && ip.ip_discovery_version)
		version = ip.ip_discovery_version;
	else
		version = ip.hw_ip_version_major << 16 | ip.hw_ip_version_minor << 8;
	major = (version >> 16) & 0xff;
	minor = (version >> 8) & 0xff;
	rev = version & 0xff;
	return major >= 6 && major <= 12 && major != 9 &&
	       !(major == 11 && minor == 5 && rev == 6);
}

static void util_last_bit64_init(struct amdgpu_device *dev, uint64_t a, uint64_t b, uint64_t *max)
{
	unsigned bit = 63;
	uint64_t x = a ^ b;

	while (bit && !(x >> bit))
		--bit;
	dev->address_prt_wa_control_bit = bit;
	*max = b ^ (1ull << bit);
}

static void va_manager_init(struct amdgpu_device *dev, uint32_t flags)
{
	const struct drm_amdgpu_info_device *i = &dev->dev_info;
	const uint64_t align = i->virtual_address_alignment;
	uint64_t start, max;

	dev->address_prt_wa_control_bit = ~0u;

	start = i->virtual_address_offset;
	max = MIN2(i->virtual_address_max, 0x100000000ull);
	vamgr_init(&dev->vamgr_32, start, max, align);

	start = max;
	if ((flags & AMDGPU_VA_MGR_RESERVE_HALF_VA_FOR_PRT) && !i->high_va_max)
		util_last_bit64_init(dev, i->virtual_address_offset, i->virtual_address_max, &max);
	else
		max = MAX2(i->virtual_address_max, 0x100000000ull);
	vamgr_init(&dev->vamgr_low, start, max, align);

	start = i->high_va_offset;
	max = MIN2(i->high_va_max, (start & ~0xffffffffull) + 0x100000000ull);
	vamgr_init(&dev->vamgr_high_32, start, max, align);

	start = max;
	if ((flags & AMDGPU_VA_MGR_RESERVE_HALF_VA_FOR_PRT) && i->high_va_max)
		util_last_bit64_init(dev, i->high_va_offset, i->high_va_max, &max);
	else
		max = MAX2(i->high_va_max, (start & ~0xffffffffull) + 0x100000000ull);
	vamgr_init(&dev->vamgr_high, start, max, align);
}

static void device_free(struct amdgpu_device *dev)
{
	if (dev == dev_list) {
		dev_list = dev->next;
	} else {
		for (struct amdgpu_device *n = dev_list; n; n = n->next) {
			if (n->next == dev) {
				n->next = dev->next;
				break;
			}
		}
	}
	close(dev->fd);
	vamgr_deinit(&dev->vamgr_32);
	vamgr_deinit(&dev->vamgr_low);
	vamgr_deinit(&dev->vamgr_high_32);
	vamgr_deinit(&dev->vamgr_high);
	pthread_mutex_destroy(&dev->bo_table_mutex);
	free(dev->bos);
	free(dev);
}

int amdgpu_device_initialize2(int fd, bool deduplicate_device, uint32_t *major_version,
			      uint32_t *minor_version, amdgpu_device_handle *device_handle)
{
	struct amdgpu_device *dev;
	drmVersionPtr version;
	uint32_t accel_working = 0;
	int r;

	*device_handle = NULL;
	if (drm_file_driver_fd(fd) < 0)
		return -EBADF;

	pthread_mutex_lock(&dev_mutex);
	if (deduplicate_device && dev_list) {
		dev = dev_list;
		dev->refcount++;
		*major_version = dev->major_version;
		*minor_version = dev->minor_version;
		*device_handle = dev;
		pthread_mutex_unlock(&dev_mutex);
		return 0;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		pthread_mutex_unlock(&dev_mutex);
		return -ENOMEM;
	}
	dev->fd = -1;
	dev->refcount = 1;

	version = drmGetVersion(fd);
	if (!version) {
		r = -EBADF;
		goto cleanup;
	}
	if (version->version_major != 3) {
		drmFreeVersion(version);
		r = -EBADF;
		goto cleanup;
	}
	dev->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	dev->major_version = (unsigned)version->version_major;
	dev->minor_version = (unsigned)version->version_minor;
	drmFreeVersion(version);
	if (dev->fd < 0) {
		r = -errno;
		goto cleanup;
	}
	pthread_mutex_init(&dev->bo_table_mutex, NULL);

	r = query_info(dev, AMDGPU_INFO_ACCEL_WORKING, sizeof(accel_working), &accel_working);
	if (r)
		goto cleanup_mutex;
	if (!accel_working) {
		r = -EBADF;
		goto cleanup_mutex;
	}
	r = query_info(dev, AMDGPU_INFO_DEV_INFO, sizeof(dev->dev_info), &dev->dev_info);
	if (r)
		goto cleanup_mutex;
	va_manager_init(dev, needs_smem_prt_wa(dev) ? AMDGPU_VA_MGR_RESERVE_HALF_VA_FOR_PRT : 0);

	*major_version = dev->major_version;
	*minor_version = dev->minor_version;
	*device_handle = dev;
	if (deduplicate_device) {
		dev->next = dev_list;
		dev_list = dev;
	}
	pthread_mutex_unlock(&dev_mutex);
	return 0;

cleanup_mutex:
	pthread_mutex_destroy(&dev->bo_table_mutex);
cleanup:
	if (dev->fd >= 0)
		close(dev->fd);
	free(dev);
	pthread_mutex_unlock(&dev_mutex);
	return r;
}

int amdgpu_device_initialize(int fd, uint32_t *major_version, uint32_t *minor_version,
			     amdgpu_device_handle *device_handle)
{
	return amdgpu_device_initialize2(fd, true, major_version, minor_version, device_handle);
}

int amdgpu_device_deinitialize(amdgpu_device_handle dev)
{
	pthread_mutex_lock(&dev_mutex);
	if (dev && !--dev->refcount)
		device_free(dev);
	pthread_mutex_unlock(&dev_mutex);
	return 0;
}

int amdgpu_device_get_fd(amdgpu_device_handle dev)
{
	return dev->fd;
}

/* Product names by PCI device and revision, from libdrm's data/amdgpu.ids
 * (MIT): the RDNA4 boards, those a Thunderbolt enclosure takes today.
 * Others get Mesa's generic name. */
static const struct {
	uint16_t device;
	uint8_t revision;
	const char *name;
} marketing_names[] = {
	{ 0x7550, 0xc0, "AMD Radeon RX 9070 XT" },
	{ 0x7550, 0xc2, "AMD Radeon RX 9070 GRE" },
	{ 0x7550, 0xc3, "AMD Radeon RX 9070" },
	{ 0x7551, 0xc0, "AMD Radeon AI Pro R9700" },
	{ 0x7551, 0xc1, "AMD Radeon AI Pro R9700S" },
	{ 0x7551, 0xc8, "AMD Radeon AI Pro R9600D" },
	{ 0x7590, 0xc0, "AMD Radeon RX 9060 XT" },
	{ 0x7590, 0xc1, "AMD Radeon RX 9060 XT LP" },
	{ 0x7590, 0xc7, "AMD Radeon RX 9060" },
	{ 0x7590, 0xcf, "AMD Radeon RX 9050" },
	{ 0x7590, 0xdf, "AMD Radeon RX 9050 4GB" },
};

const char *amdgpu_get_marketing_name(amdgpu_device_handle dev)
{
	for (size_t i = 0; i < sizeof(marketing_names) / sizeof(marketing_names[0]); ++i)
		if (marketing_names[i].device == dev->dev_info.device_id &&
		    marketing_names[i].revision == dev->dev_info.pci_rev)
			return marketing_names[i].name;
	return NULL;
}

int amdgpu_query_sw_info(amdgpu_device_handle dev, enum amdgpu_sw_info info, void *value)
{
	uint32_t *val32 = value;

	switch (info) {
	case amdgpu_sw_info_address32_hi:
		if (dev->vamgr_high_32.va_max)
			*val32 = (uint32_t)((dev->vamgr_high_32.va_max - 1) >> 32);
		else
			*val32 = (uint32_t)((dev->vamgr_32.va_max - 1) >> 32);
		return 0;
	case amdgpu_sw_info_address_prt_wa_control_bit:
		*val32 = dev->address_prt_wa_control_bit;
		return 0;
	}
	return -EINVAL;
}

/* ---- virtual address ranges ---- */

int amdgpu_va_range_query(amdgpu_device_handle dev, enum amdgpu_gpu_va_range type,
			  uint64_t *start, uint64_t *end)
{
	if (type != amdgpu_gpu_va_range_general)
		return -EINVAL;
	*start = dev->dev_info.virtual_address_offset;
	*end = dev->dev_info.virtual_address_max;
	return 0;
}

int amdgpu_va_range_alloc(amdgpu_device_handle dev, enum amdgpu_gpu_va_range va_range_type,
			  uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
			  uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle,
			  uint64_t flags)
{
	const bool search_from_top = !!(flags & AMDGPU_VA_RANGE_REPLAYABLE);
	struct va_mgr *vamgr;
	int r;

	(void)va_range_type;
	if ((flags & AMDGPU_VA_RANGE_HIGH) && !dev->vamgr_high_32.va_max)
		flags &= ~(uint64_t)AMDGPU_VA_RANGE_HIGH;
	if (flags & AMDGPU_VA_RANGE_HIGH)
		vamgr = (flags & AMDGPU_VA_RANGE_32_BIT) ? &dev->vamgr_high_32 : &dev->vamgr_high;
	else
		vamgr = (flags & AMDGPU_VA_RANGE_32_BIT) ? &dev->vamgr_32 : &dev->vamgr_low;

	va_base_alignment = MAX2(va_base_alignment, vamgr->va_alignment);
	size = ALIGN(size, vamgr->va_alignment);
	r = vamgr_find_va(vamgr, size, va_base_alignment, va_base_required, search_from_top,
			  va_base_allocated);
	if (!(flags & AMDGPU_VA_RANGE_32_BIT) && r) {
		/* Fall back to the 32-bit range. */
		vamgr = (flags & AMDGPU_VA_RANGE_HIGH) ? &dev->vamgr_high_32 : &dev->vamgr_32;
		r = vamgr_find_va(vamgr, size, va_base_alignment, va_base_required,
				  search_from_top, va_base_allocated);
	}
	if (!r) {
		struct amdgpu_va *va = calloc(1, sizeof(*va));

		if (!va) {
			vamgr_free_va(vamgr, *va_base_allocated, size);
			return -ENOMEM;
		}
		va->address = *va_base_allocated;
		va->size = size;
		va->vamgr = vamgr;
		*va_range_handle = va;
	}
	return r;
}

int amdgpu_va_range_free(amdgpu_va_handle va)
{
	if (!va || !va->address)
		return 0;
	vamgr_free_va(va->vamgr, va->address, va->size);
	free(va);
	return 0;
}

uint64_t amdgpu_va_get_start_addr(amdgpu_va_handle va)
{
	return va->address;
}

/* ---- buffers (amdgpu_bo.c) ---- */

static int bo_table_insert(struct amdgpu_device *dev, uint32_t handle, struct amdgpu_bo *bo)
{
	if (handle >= dev->bos_cap) {
		uint32_t cap = dev->bos_cap ? dev->bos_cap : 256;
		struct amdgpu_bo **grown;

		while (cap <= handle)
			cap *= 2;
		grown = realloc(dev->bos, cap * sizeof(*grown));
		if (!grown)
			return -ENOMEM;
		memset(grown + dev->bos_cap, 0, (cap - dev->bos_cap) * sizeof(*grown));
		dev->bos = grown;
		dev->bos_cap = cap;
	}
	dev->bos[handle] = bo;
	return 0;
}

static struct amdgpu_bo *bo_table_lookup(struct amdgpu_device *dev, uint32_t handle)
{
	return handle < dev->bos_cap ? dev->bos[handle] : NULL;
}

static int bo_create(struct amdgpu_device *dev, uint64_t size, uint32_t handle,
		     amdgpu_bo_handle *out)
{
	struct amdgpu_bo *bo = calloc(1, sizeof(*bo));
	int r;

	if (!bo)
		return -ENOMEM;
	r = bo_table_insert(dev, handle, bo);
	if (r) {
		free(bo);
		return r;
	}
	bo->refcount = 1;
	bo->dev = dev;
	bo->alloc_size = size;
	bo->handle = handle;
	pthread_mutex_init(&bo->cpu_access_mutex, NULL);
	*out = bo;
	return 0;
}

int amdgpu_bo_alloc(amdgpu_device_handle dev, struct amdgpu_bo_alloc_request *alloc_buffer,
		    amdgpu_bo_handle *buf_handle)
{
	union drm_amdgpu_gem_create args;
	int r;

	if (!alloc_buffer || !buf_handle)
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.in.bo_size = alloc_buffer->alloc_size;
	args.in.alignment = alloc_buffer->phys_alignment;
	args.in.domains = alloc_buffer->preferred_heap;
	args.in.domain_flags = alloc_buffer->flags;
	r = drmCommandWriteRead(dev->fd, DRM_AMDGPU_GEM_CREATE, &args, sizeof(args));
	if (r)
		return r;
	pthread_mutex_lock(&dev->bo_table_mutex);
	r = bo_create(dev, alloc_buffer->alloc_size, args.out.handle, buf_handle);
	pthread_mutex_unlock(&dev->bo_table_mutex);
	if (r)
		drmCloseBufferHandle(dev->fd, args.out.handle);
	return r;
}

int amdgpu_bo_set_metadata(amdgpu_bo_handle bo, struct amdgpu_bo_metadata *info)
{
	struct drm_amdgpu_gem_metadata args;

	if (!info || info->size_metadata > sizeof(args.data.data))
		return -EINVAL;
	memset(&args, 0, sizeof(args));
	args.handle = bo->handle;
	args.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
	args.data.flags = info->flags;
	args.data.tiling_info = info->tiling_info;
	if (info->size_metadata) {
		args.data.data_size_bytes = info->size_metadata;
		memcpy(args.data.data, info->umd_metadata, info->size_metadata);
	}
	return drmCommandWriteRead(bo->dev->fd, DRM_AMDGPU_GEM_METADATA, &args, sizeof(args));
}

int amdgpu_bo_query_info(amdgpu_bo_handle bo, struct amdgpu_bo_info *info)
{
	struct drm_amdgpu_gem_metadata metadata;
	struct drm_amdgpu_gem_create_in bo_info;
	struct drm_amdgpu_gem_op gem_op;
	int r;

	if (!bo->handle || !info)
		return -EINVAL;
	memset(&metadata, 0, sizeof(metadata));
	metadata.handle = bo->handle;
	metadata.op = AMDGPU_GEM_METADATA_OP_GET_METADATA;
	r = drmCommandWriteRead(bo->dev->fd, DRM_AMDGPU_GEM_METADATA, &metadata, sizeof(metadata));
	if (r)
		return r;
	if (metadata.data.data_size_bytes > sizeof(info->metadata.umd_metadata))
		return -EINVAL;
	memset(&bo_info, 0, sizeof(bo_info));
	memset(&gem_op, 0, sizeof(gem_op));
	gem_op.handle = bo->handle;
	gem_op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
	gem_op.value = (uintptr_t)&bo_info;
	r = drmCommandWriteRead(bo->dev->fd, DRM_AMDGPU_GEM_OP, &gem_op, sizeof(gem_op));
	if (r)
		return r;
	memset(info, 0, sizeof(*info));
	info->alloc_size = bo_info.bo_size;
	info->phys_alignment = bo_info.alignment;
	info->preferred_heap = bo_info.domains;
	info->alloc_flags = bo_info.domain_flags;
	info->metadata.flags = metadata.data.flags;
	info->metadata.tiling_info = metadata.data.tiling_info;
	info->metadata.size_metadata = metadata.data.data_size_bytes;
	if (metadata.data.data_size_bytes)
		memcpy(info->metadata.umd_metadata, metadata.data.data,
		       metadata.data.data_size_bytes);
	return 0;
}

int amdgpu_bo_export(amdgpu_bo_handle bo, enum amdgpu_bo_handle_type type,
		     uint32_t *shared_handle)
{
	switch (type) {
	case amdgpu_bo_handle_type_gem_flink_name:
		/* GEM_FLINK is not render-node API. */
		return -EPERM;
	case amdgpu_bo_handle_type_kms:
	case amdgpu_bo_handle_type_kms_noimport:
		*shared_handle = bo->handle;
		return 0;
	case amdgpu_bo_handle_type_dma_buf_fd:
		return drmPrimeHandleToFD(bo->dev->fd, bo->handle, DRM_CLOEXEC | DRM_RDWR,
					  (int *)shared_handle) ? -errno : 0;
	}
	return -EINVAL;
}

int amdgpu_bo_import(amdgpu_device_handle dev, enum amdgpu_bo_handle_type type,
		     uint32_t shared_handle, struct amdgpu_bo_import_result *output)
{
	struct drm_amdgpu_gem_create_in info;
	struct drm_amdgpu_gem_op op;
	struct amdgpu_bo *bo;
	uint32_t handle = 0;
	int r;

	if (type != amdgpu_bo_handle_type_dma_buf_fd)
		return type == amdgpu_bo_handle_type_gem_flink_name ? -EPERM :
		       type == amdgpu_bo_handle_type_kms ||
		       type == amdgpu_bo_handle_type_kms_noimport ? -EPERM : -EINVAL;

	pthread_mutex_lock(&dev->bo_table_mutex);
	if (drmPrimeFDToHandle(dev->fd, (int)shared_handle, &handle)) {
		r = -errno;
		goto unlock;
	}
	bo = bo_table_lookup(dev, handle);
	if (bo) {
		bo->refcount++;
		pthread_mutex_unlock(&dev->bo_table_mutex);
		output->buf_handle = bo;
		output->alloc_size = bo->alloc_size;
		return 0;
	}
	memset(&info, 0, sizeof(info));
	memset(&op, 0, sizeof(op));
	op.handle = handle;
	op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
	op.value = (uintptr_t)&info;
	r = drmCommandWriteRead(dev->fd, DRM_AMDGPU_GEM_OP, &op, sizeof(op));
	if (!r)
		r = bo_create(dev, info.bo_size, handle, &bo);
	if (r) {
		drmCloseBufferHandle(dev->fd, handle);
		goto unlock;
	}
	output->buf_handle = bo;
	output->alloc_size = bo->alloc_size;
unlock:
	pthread_mutex_unlock(&dev->bo_table_mutex);
	return r;
}

int amdgpu_bo_cpu_unmap(amdgpu_bo_handle bo)
{
	int r;

	pthread_mutex_lock(&bo->cpu_access_mutex);
	if (bo->cpu_map_count == 0) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return -EINVAL;
	}
	if (--bo->cpu_map_count > 0) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return 0;
	}
	r = drmFileMunmap(bo->cpu_ptr, bo->alloc_size) == 0 ? 0 : -errno;
	bo->cpu_ptr = NULL;
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	return r;
}

int amdgpu_bo_free(amdgpu_bo_handle bo)
{
	struct amdgpu_device *dev = bo->dev;

	pthread_mutex_lock(&dev->bo_table_mutex);
	if (--bo->refcount == 0) {
		if (bo->handle < dev->bos_cap && dev->bos[bo->handle] == bo)
			dev->bos[bo->handle] = NULL;
		/* The mapping goes before the handle (the driver frees the
		 * object with its last handle). */
		if (bo->cpu_map_count > 0) {
			bo->cpu_map_count = 1;
			amdgpu_bo_cpu_unmap(bo);
		}
		drmCloseBufferHandle(dev->fd, bo->handle);
		pthread_mutex_destroy(&bo->cpu_access_mutex);
		free(bo);
	}
	pthread_mutex_unlock(&dev->bo_table_mutex);
	return 0;
}

void amdgpu_bo_inc_ref(amdgpu_bo_handle bo)
{
	pthread_mutex_lock(&bo->dev->bo_table_mutex);
	bo->refcount++;
	pthread_mutex_unlock(&bo->dev->bo_table_mutex);
}

uint32_t amdgpu_bo_get_handle(amdgpu_bo_handle bo)
{
	return bo->handle;
}

int amdgpu_bo_cpu_map(amdgpu_bo_handle bo, void **cpu)
{
	union drm_amdgpu_gem_mmap args;
	void *ptr;
	int r;

	pthread_mutex_lock(&bo->cpu_access_mutex);
	if (bo->cpu_ptr) {
		bo->cpu_map_count++;
		*cpu = bo->cpu_ptr;
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return 0;
	}
	memset(&args, 0, sizeof(args));
	args.in.handle = bo->handle;
	r = drmCommandWriteRead(bo->dev->fd, DRM_AMDGPU_GEM_MMAP, &args, sizeof(args));
	if (r) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return r;
	}
	ptr = drmFileMmap(NULL, bo->alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->dev->fd,
			  (off_t)args.out.addr_ptr);
	if (ptr == MAP_FAILED) {
		pthread_mutex_unlock(&bo->cpu_access_mutex);
		return -errno;
	}
	bo->cpu_ptr = ptr;
	bo->cpu_map_count = 1;
	pthread_mutex_unlock(&bo->cpu_access_mutex);
	*cpu = ptr;
	return 0;
}

int amdgpu_create_bo_from_user_mem(amdgpu_device_handle dev, void *cpu, uint64_t size,
				   amdgpu_bo_handle *buf_handle)
{
	(void)dev;
	(void)cpu;
	(void)size;
	(void)buf_handle;
	return -ENOSYS;
}
