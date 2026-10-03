/* libmlg_drm end to end: a render-node client issuing what Mesa RADV's
 * amdgpu layer issues, with Linux uapi structures and request numbers,
 * through mlg_open/mlg_ioctl/mlg_mmap, carried by the Linux-file core
 * (lx_loopback.c) to the unmodified upstream DRM/amdgpu of the fixture
 * device (linuxu/tests/cs_fixture.c), whose software GPU runs the work.
 *
 * Covers: version and caps, AMDGPU_INFO, contexts, GEM create/VA/mmap,
 * a BO list handle (AMDGPU_BO_LIST), AMDGPU_CS of PM4 on compute with a
 * syncobj out, waits through the async path (WAIT_CS, WAIT_FENCES,
 * SYNCOBJ_WAIT, timeline query), syncobj and fence export as file
 * descriptors of the driver process (HANDLE_TO_FD, FD_TO_HANDLE,
 * FENCE_TO_HANDLE), and teardown. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "mlg_drm.h"
#include "mlg_uapi.h"
#include "lx_loopback.h"

struct pci_dev *cs_fixture_init(void);
void cs_fixture_stop(void);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", \
	__FILE__, __LINE__, #c, errno); abort(); } } while (0)

/* PM4 type-3 WRITE_DATA to memory (dst_sel 5, write confirm), as RADV
 * emits it. */
#define PKT3(op, count)	((3u << 30) | (((op) & 0xffu) << 8) | (((count) & 0x3fffu) << 16))
#define PKT3_WRITE_DATA	0x37u
#define PKT3_NOP	0x10u

static int64_t deadline_ns(unsigned int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + (int64_t)ms * 1000000ll;
}

static uint32_t gem_create(int fd, uint64_t size, uint32_t domain, uint64_t flags)
{
	union drm_amdgpu_gem_create c = { .in = { .bo_size = size, .alignment = 4096,
						  .domains = domain, .domain_flags = flags } };

	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &c) == 0);
	return c.out.handle;
}

static void gem_va(int fd, uint32_t handle, uint32_t op, uint64_t va, uint64_t size)
{
	struct drm_amdgpu_gem_va v = { .handle = handle, .operation = op, .va_address = va,
		.map_size = size,
		.flags = op == AMDGPU_VA_OP_MAP ? AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE |
						  AMDGPU_VM_PAGE_EXECUTABLE : 0 };

	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_VA, &v) == 0);
}

static void *gem_map(int fd, uint32_t handle, size_t size)
{
	union drm_amdgpu_gem_mmap m = { .in = { .handle = handle } };
	void *p;

	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &m) == 0);
	p = mlg_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.out.addr_ptr);
	CHECK(p != MAP_FAILED);
	return p;
}

int main(void)
{
	struct mlg_transport transport;
	struct pci_dev *pdev = cs_fixture_init();
	const size_t bo_size = 64 << 10;
	int fd;

	CHECK(!lx_loopback_transport(pdev, &transport));
	CHECK(!mlg_drm_set_transport(&transport));
	fd = mlg_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);

	/* drmGetVersion, drmGetCap. */
	char name[16] = {0};
	struct drm_version version = { .name_len = sizeof(name) - 1, .name = name };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_VERSION, &version) == 0);
	CHECK(!strcmp(name, "amdgpu") && version.version_major == 3);
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);

	/* ac_query_gpu_info's first queries. */
	struct drm_amdgpu_info_device dev = {0};
	struct drm_amdgpu_info info = { .return_pointer = (uint64_t)(uintptr_t)&dev,
		.return_size = sizeof(dev), .query = AMDGPU_INFO_DEV_INFO };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &info) == 0 && dev.family == AMDGPU_FAMILY_GC_12_0_0);
	struct drm_amdgpu_memory_info mem = {0};
	info = (struct drm_amdgpu_info){ .return_pointer = (uint64_t)(uintptr_t)&mem,
		.return_size = sizeof(mem), .query = AMDGPU_INFO_MEMORY };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &info) == 0 && mem.vram.total_heap_size);
	struct drm_amdgpu_info_hw_ip ip = {0};
	info = (struct drm_amdgpu_info){ .return_pointer = (uint64_t)(uintptr_t)&ip,
		.return_size = sizeof(ip), .query = AMDGPU_INFO_HW_IP_INFO,
		.query_hw_ip = { .type = AMDGPU_HW_IP_COMPUTE } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &info) == 0 && ip.available_rings == 1);

	/* A context, syncobjs. */
	union drm_amdgpu_ctx ctx = { .in = { .op = AMDGPU_CTX_OP_ALLOC_CTX } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx) == 0);
	const uint32_t ctx_id = ctx.out.alloc.ctx_id;
	struct drm_syncobj_create sc = { .flags = 0 };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc) == 0);
	const uint32_t done = sc.handle;

	/* Buffers: data (GTT), IB (GTT), VRAM; a VA for each, two mapped. */
	const uint32_t data = gem_create(fd, bo_size, AMDGPU_GEM_DOMAIN_GTT, 0);
	const uint32_t ibbo = gem_create(fd, bo_size, AMDGPU_GEM_DOMAIN_GTT, 0);
	const uint32_t vram = gem_create(fd, bo_size, AMDGPU_GEM_DOMAIN_VRAM,
					 AMDGPU_GEM_CREATE_NO_CPU_ACCESS);
	const uint64_t va = (dev.virtual_address_offset + (8ull << 20)) & ~((2ull << 20) - 1);
	gem_va(fd, data, AMDGPU_VA_OP_MAP, va, bo_size);
	gem_va(fd, ibbo, AMDGPU_VA_OP_MAP, va + (2 << 20), bo_size);
	gem_va(fd, vram, AMDGPU_VA_OP_MAP, va + (4 << 20), bo_size);
	uint32_t *cpu = gem_map(fd, data, bo_size);
	uint32_t *ib = gem_map(fd, ibbo, bo_size);
	memset(cpu, 0, bo_size);

	/* A BO list handle, as older winsys code uses. */
	struct drm_amdgpu_bo_list_entry entries[3] = { { data, 0 }, { ibbo, 0 }, { vram, 0 } };
	union drm_amdgpu_bo_list bl = { .in = { .operation = AMDGPU_BO_LIST_OP_CREATE,
		.bo_number = 3, .bo_info_size = sizeof(entries[0]),
		.bo_info_ptr = (uint64_t)(uintptr_t)entries } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &bl) == 0);
	const uint32_t list = bl.out.list_handle;

	/* AMDGPU_CS: two WRITE_DATA on compute, signaling the syncobj. */
	uint32_t n = 0;
	for (uint32_t i = 0; i < 2; ++i) {
		const uint64_t dst = va + i * 64;

		ib[n++] = PKT3(PKT3_WRITE_DATA, 3);
		ib[n++] = (5u << 8) | (1u << 20);
		ib[n++] = (uint32_t)dst;
		ib[n++] = (uint32_t)(dst >> 32);
		ib[n++] = 0xfeed0000u + i;
	}
	while (n % 8)
		ib[n++] = PKT3(PKT3_NOP, 0x3fff);
	struct drm_amdgpu_cs_chunk_ib chunk_ib = { .ip_type = AMDGPU_HW_IP_COMPUTE,
		.va_start = va + (2 << 20), .ib_bytes = n * 4 };
	struct drm_amdgpu_cs_chunk_sem out_sem = { .handle = done };
	struct drm_amdgpu_cs_chunk chunks[2] = {
		{ AMDGPU_CHUNK_ID_IB, sizeof(chunk_ib) / 4, (uint64_t)(uintptr_t)&chunk_ib },
		{ AMDGPU_CHUNK_ID_SYNCOBJ_OUT, sizeof(out_sem) / 4, (uint64_t)(uintptr_t)&out_sem },
	};
	uint64_t chunk_ptrs[2] = { (uint64_t)(uintptr_t)&chunks[0], (uint64_t)(uintptr_t)&chunks[1] };
	union drm_amdgpu_cs cs = { .in = { .ctx_id = ctx_id, .bo_list_handle = list,
		.num_chunks = 2, .chunks = (uint64_t)(uintptr_t)chunk_ptrs } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_CS, &cs) == 0);
	const uint64_t seq = cs.out.handle;
	CHECK(seq >= 1);

	/* amdgpu_cs_query_fence_status: a timed wait (async) ... */
	unsigned int before = lx_loopback_async_calls();
	union drm_amdgpu_wait_cs wcs = { .in = { .handle = seq, .ip_type = AMDGPU_HW_IP_COMPUTE,
		.ctx_id = ctx_id, .timeout = (uint64_t)deadline_ns(2000) } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &wcs) == 0 && wcs.out.status == 0);
	CHECK(lx_loopback_async_calls() == before + 1);
	CHECK(cpu[0] == 0xfeed0000u && cpu[16] == 0xfeed0001u);
	/* ... and a poll (no worker). */
	wcs = (union drm_amdgpu_wait_cs){ .in = { .handle = seq, .ip_type = AMDGPU_HW_IP_COMPUTE,
		.ctx_id = ctx_id, .timeout = 0 } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &wcs) == 0 && wcs.out.status == 0);
	CHECK(lx_loopback_async_calls() == before + 1);

	/* AMDGPU_WAIT_FENCES: the fence array behind the block. */
	struct drm_amdgpu_fence fences[1] = { { .ctx_id = ctx_id, .ip_type = AMDGPU_HW_IP_COMPUTE,
		.seq_no = seq } };
	union drm_amdgpu_wait_fences wf = { .in = { .fences = (uint64_t)(uintptr_t)fences,
		.fence_count = 1, .wait_all = 1, .timeout_ns = (uint64_t)deadline_ns(2000) } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_WAIT_FENCES, &wf) == 0 && wf.out.status == 1);

	/* The syncobj the CS signaled. */
	uint32_t handles[1] = { done };
	struct drm_syncobj_wait sw = { .handles = (uint64_t)(uintptr_t)handles, .count_handles = 1,
		.timeout_nsec = deadline_ns(2000), .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == 0);
	uint64_t points[1] = { 99 };
	struct drm_syncobj_timeline_array query = { .handles = (uint64_t)(uintptr_t)handles,
		.points = (uint64_t)(uintptr_t)points, .count_handles = 1 };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &query) == 0 && points[0] == 0);

	/* Export and import: file descriptors of the driver process. */
	const unsigned int files = lx_loopback_open_files();
	struct drm_syncobj_handle sh = { .handle = done };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sh) == 0 && sh.fd > fd);
	CHECK(lx_loopback_open_files() == files + 1);
	struct drm_syncobj_handle back = { .fd = sh.fd };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &back) == 0 && back.handle &&
	      back.handle != done);
	union drm_amdgpu_fence_to_handle fh = { .in = { .fence = { .ctx_id = ctx_id,
		.ip_type = AMDGPU_HW_IP_COMPUTE, .seq_no = seq },
		.what = AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE, &fh) == 0 && fh.out.handle);
	/* The exported descriptor is not a device: no requests; it closes
	 * like any descriptor of the process. */
	CHECK(mlg_ioctl(sh.fd, DRM_IOCTL_VERSION, &version) == -1 && errno == EBADF);
	CHECK(mlg_close(sh.fd) == 0 && lx_loopback_open_files() == files);

	/* sync_file: export the syncobj's fence, import it into another
	 * syncobj (what WSI and external semaphores do), and the CS fence as
	 * a sync_file directly. */
	struct drm_syncobj_handle sf = { .handle = done,
		.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &sf) == 0 && sf.fd >= 0);
	struct drm_syncobj_create sc2 = { 0 };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc2) == 0);
	struct drm_syncobj_handle imp = { .handle = sc2.handle, .fd = sf.fd,
		.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp) == 0);
	handles[0] = sc2.handle;
	sw = (struct drm_syncobj_wait){ .handles = (uint64_t)(uintptr_t)handles, .count_handles = 1,
		.timeout_nsec = deadline_ns(2000), .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw) == 0);
	CHECK(mlg_close(sf.fd) == 0);
	union drm_amdgpu_fence_to_handle fs = { .in = { .fence = { .ctx_id = ctx_id,
		.ip_type = AMDGPU_HW_IP_COMPUTE, .seq_no = seq },
		.what = AMDGPU_FENCE_TO_HANDLE_GET_SYNC_FILE_FD } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE, &fs) == 0);
	CHECK(mlg_close((int)fs.out.handle) == 0 && lx_loopback_open_files() == files);
	struct drm_syncobj_destroy sd2 = { .handle = sc2.handle };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd2) == 0);
	/* linuxu has no eventfd (CONFIG_EVENTFD=n): DRM_IOCTL_SYNCOBJ_EVENTFD
	 * fails as on a Linux kernel built without it. */
	struct drm_syncobj_eventfd ev = { .handle = done, .fd = 0 };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &ev) == -1);

	/* Teardown, as a winsys does. */
	CHECK(mlg_munmap(cpu, bo_size) == 0 && mlg_munmap(ib, bo_size) == 0);
	gem_va(fd, data, AMDGPU_VA_OP_UNMAP, va, bo_size);
	gem_va(fd, ibbo, AMDGPU_VA_OP_UNMAP, va + (2 << 20), bo_size);
	gem_va(fd, vram, AMDGPU_VA_OP_UNMAP, va + (4 << 20), bo_size);
	bl = (union drm_amdgpu_bo_list){ .in = { .operation = AMDGPU_BO_LIST_OP_DESTROY,
					       .list_handle = list } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &bl) == 0);
	const uint32_t bos[3] = { data, ibbo, vram };
	for (int i = 0; i < 3; ++i) {
		struct drm_gem_close gc = { .handle = bos[i] };

		CHECK(mlg_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
	}
	const uint32_t syncobjs[3] = { done, back.handle, fh.out.handle };
	for (int i = 0; i < 3; ++i) {
		struct drm_syncobj_destroy sd = { .handle = syncobjs[i] };

		CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &sd) == 0);
	}
	ctx = (union drm_amdgpu_ctx){ .in = { .op = AMDGPU_CTX_OP_FREE_CTX, .ctx_id = ctx_id } };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx) == 0);
	CHECK(mlg_close(fd) == 0);
	lx_loopback_exit();
	cs_fixture_stop();
	puts("PASS libmlg_drm CS: version, caps, INFO, ctx, GEM create/VA/mmap, BO list, "
	     "AMDGPU_CS on compute, WAIT_CS/WAIT_FENCES/SYNCOBJ_WAIT through async workers, "
	     "syncobj, sync_file and fence export as driver-process descriptors, teardown");
	return 0;
}
