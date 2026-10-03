/* libdrm-mlg end to end: the libdrm and libdrm_amdgpu API a Mesa driver
 * uses, built as such a driver builds it (this platform's <drm.h>, BSD
 * request encoding), through libmlg_drm and the loopback transport into the
 * unmodified upstream DRM/amdgpu of the CS fixture device.
 *
 * Covers: the device list and node paths, DRM files as descriptors of this
 * process (dup, close-on-last-copy release, stat and open by path),
 * version and caps, syncobjs exported and imported as descriptors,
 * sync_file wait and merge, amdgpu devices (deduplication), GPU virtual
 * address ranges, buffers (create, map, metadata, dma-buf export and
 * import), and the mmap/munmap wrappers.
 *
 * This file is a library of its own, linked against libdrm-mlg: the
 * kernel objects of the test program define some of the same names
 * (amdgpu_bo_set_metadata), and a library binds to the libraries it was
 * linked with. test_libdrm_mlg_main.c brings the fixture up and calls
 * test_libdrm_mlg(). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xf86drm.h>
#include <amdgpu_drm.h>
#include <amdgpu.h>
#include <libsync.h>

#include "mlg_drm.h"
#include "cs_fixture.h"
#include "lx_loopback.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s (errno %d)\n", \
	__FILE__, __LINE__, #c, errno); exit(1); } } while (0)

/* The driver releases a file once the watcher has seen its last copy
 * closed: wait for the open-file count to reach @want. */
static int files_reach(unsigned int want)
{
	for (int i = 0; i < 200; ++i) {
		if (lx_loopback_open_files() == want)
			return 1;
		usleep(10000);
	}
	return 0;
}

int test_libdrm_mlg(void);

int test_libdrm_mlg(void)
{
	const unsigned int base = lx_loopback_open_files();

	/* The device list: one PCI device with a render node. */
	drmDevicePtr devices[4];
	CHECK(drmGetDevices2(0, NULL, 0) == 1);
	CHECK(drmGetDevices2(0, devices, 4) == 1);
	drmDevicePtr dev = devices[0];
	CHECK(dev->bustype == DRM_BUS_PCI && dev->available_nodes == 1 << DRM_NODE_RENDER);
	CHECK(!strcmp(dev->nodes[DRM_NODE_RENDER], "/dev/dri/renderD128"));
	CHECK(dev->deviceinfo.pci->vendor_id == 0x1002 && dev->deviceinfo.pci->device_id == 0x7551);
	CHECK(dev->businfo.pci->bus == 0xc3 && dev->deviceinfo.pci->revision_id == 0xc0);

	/* Nodes by path. */
	struct stat st;
	CHECK(drmFileStat(dev->nodes[DRM_NODE_RENDER], &st) == 0 && S_ISCHR(st.st_mode));
	CHECK(major(st.st_rdev) == 226 && minor(st.st_rdev) == 128);
	drmDevicePtr by_id;
	CHECK(drmGetDeviceFromDevId(st.st_rdev, 0, &by_id) == 0 && drmDevicesEqual(by_id, dev));
	drmFreeDevice(&by_id);
	CHECK(drmFileStat("/dev/dri/card0", &st) == -1 && errno == ENOENT);
	CHECK(drmFileOpen("/dev/dri/card0", O_RDWR) == -1 && errno == ENOENT);
	CHECK(drmFileStat("/", &st) == 0 && S_ISDIR(st.st_mode));
	int null_fd = drmFileOpen("/dev/null", O_RDONLY | O_CLOEXEC);
	CHECK(null_fd >= 0 && drmIoctl(null_fd, DRM_IOCTL_VERSION, NULL) == -1 && errno == EBADF);
	close(null_fd);

	int fd = drmFileOpen(dev->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0 && lx_loopback_open_files() == base + 1);
	CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	CHECK(drmGetNodeTypeFromFd(fd) == DRM_NODE_RENDER);
	char *name = drmGetRenderDeviceNameFromFd(fd);
	CHECK(name && !strcmp(name, "/dev/dri/renderD128"));
	free(name);
	CHECK(drmGetPrimaryDeviceNameFromFd(fd) == NULL);
	drmDevicePtr of_fd;
	CHECK(drmGetDevice2(fd, 0, &of_fd) == 0 && drmDevicesEqual(of_fd, dev));
	drmFreeDevice(&of_fd);
	drmFreeDevices(devices, 1);

	/* Version and caps. */
	drmVersionPtr v = drmGetVersion(fd);
	CHECK(v && !strcmp(v->name, "amdgpu") && v->version_major == 3);
	drmFreeVersion(v);
	uint64_t cap = 0;
	CHECK(drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1);

	/* A copy keeps the file open; the last close releases it. */
	int copy = dup(fd);
	CHECK(copy >= 0 && drmGetNodeTypeFromFd(copy) == DRM_NODE_RENDER);
	int other = drmFileOpen(DRM_DIR_NAME "/renderD128", O_RDWR);
	CHECK(other >= 0 && lx_loopback_open_files() == base + 2);
	close(other);
	CHECK(files_reach(base + 1));
	CHECK(drmGetNodeTypeFromFd(other) == -1);

	/* Syncobjs, and their descriptors. */
	uint32_t a, b, c;
	CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &a) == 0);
	CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &b) == 0);
	CHECK(drmSyncobjWait(fd, &a, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
	int obj_fd;
	CHECK(drmSyncobjHandleToFD(fd, a, &obj_fd) == 0 && obj_fd > copy);
	CHECK(lx_loopback_open_files() == base + 2);
	CHECK(drmSyncobjFDToHandle(fd, obj_fd, &c) == 0 && c != a);
	close(obj_fd);
	CHECK(files_reach(base + 1));
	CHECK(drmSyncobjFDToHandle(fd, obj_fd, &c) == -1 && errno == EBADF);
	uint64_t point = 99;
	CHECK(drmSyncobjQuery(fd, &a, &point, 1) == 0 && point == 0);

	/* sync_files: export, wait, merge, accumulate, import. */
	int sf_a, sf_b;
	CHECK(drmSyncobjExportSyncFile(fd, a, &sf_a) == 0);
	CHECK(drmSyncobjExportSyncFile(fd, b, &sf_b) == 0);
	CHECK(sync_wait(sf_a, 1000) == 0);
	int merged = sync_merge("test", sf_a, sf_b);
	CHECK(merged >= 0 && sync_wait(merged, 1000) == 0);
	int acc = -1;
	CHECK(sync_accumulate("test", &acc, sf_a) == 0 && acc >= 0);
	CHECK(sync_accumulate("test", &acc, sf_b) == 0 && sync_wait(acc, 0) == 0);
	CHECK(drmSyncobjCreate(fd, 0, &c) == 0);
	CHECK(drmSyncobjImportSyncFile(fd, c, merged) == 0);
	CHECK(drmSyncobjWait(fd, &c, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
	CHECK(sync_wait(0, 0) == -1 && errno == EINVAL);
	close(sf_a);
	close(sf_b);
	close(merged);
	close(acc);
	CHECK(drmSyncobjDestroy(fd, a) == 0 && drmSyncobjDestroy(fd, b) == 0 &&
	      drmSyncobjDestroy(fd, c) == 0);

	/* amdgpu devices: one per GPU, whatever the descriptor. */
	uint32_t major, minor;
	amdgpu_device_handle adev, again;
	CHECK(amdgpu_device_initialize(fd, &major, &minor, &adev) == 0 && major == 3);
	CHECK(amdgpu_device_initialize(copy, &major, &minor, &again) == 0 && again == adev);
	CHECK(amdgpu_device_deinitialize(again) == 0);
	int dfd = amdgpu_device_get_fd(adev);
	CHECK(dfd != fd && drmGetNodeTypeFromFd(dfd) == DRM_NODE_RENDER);
	uint32_t hi = 0;
	CHECK(amdgpu_query_sw_info(adev, amdgpu_sw_info_address32_hi, &hi) == 0);
	const char *product = amdgpu_get_marketing_name(adev);
	CHECK(product && !strcmp(product, "AMD Radeon AI Pro R9700"));

	/* GPU virtual address ranges. */
	uint64_t start, end, va, va32, again_va;
	amdgpu_va_handle vh, vh32, vh2;
	CHECK(amdgpu_va_range_query(adev, amdgpu_gpu_va_range_general, &start, &end) == 0 &&
	      end > start);
	CHECK(amdgpu_va_range_alloc(adev, amdgpu_gpu_va_range_general, 1 << 20, 1 << 16, 0, &va,
				    &vh, 0) == 0 && va >= start && va % (1 << 16) == 0);
	CHECK(amdgpu_va_range_alloc(adev, amdgpu_gpu_va_range_general, 1 << 16, 0, 0, &va32,
				    &vh32, AMDGPU_VA_RANGE_32_BIT) == 0 &&
	      va32 >> 32 == 0 && va32 + (1 << 16) <= 0x100000000ull);
	CHECK(amdgpu_va_range_free(vh) == 0);
	CHECK(amdgpu_va_range_alloc(adev, amdgpu_gpu_va_range_general, 1 << 20, 1 << 16, 0,
				    &again_va, &vh2, 0) == 0 && again_va == va);
	CHECK(amdgpu_va_get_start_addr(vh2) == va);
	CHECK(amdgpu_va_range_free(vh2) == 0 && amdgpu_va_range_free(vh32) == 0);

	/* Buffers. */
	struct amdgpu_bo_alloc_request req = { .alloc_size = 64 << 10, .phys_alignment = 4096,
		.preferred_heap = AMDGPU_GEM_DOMAIN_GTT };
	amdgpu_bo_handle bo;
	CHECK(amdgpu_bo_alloc(adev, &req, &bo) == 0);
	uint32_t *cpu, *cpu2;
	CHECK(amdgpu_bo_cpu_map(bo, (void **)&cpu) == 0);
	CHECK(amdgpu_bo_cpu_map(bo, (void **)&cpu2) == 0 && cpu2 == cpu);
	cpu[0] = 0x600df00d;
	cpu[(64 << 10) / 4 - 1] = 0xfeedface;
	CHECK(amdgpu_bo_cpu_unmap(bo) == 0 && amdgpu_bo_cpu_unmap(bo) == 0);
	CHECK(amdgpu_bo_cpu_unmap(bo) == -EINVAL);
	struct amdgpu_bo_metadata md = { .flags = 1, .tiling_info = 0x55, .size_metadata = 4,
		.umd_metadata = { 0x12345678 } };
	CHECK(amdgpu_bo_set_metadata(bo, &md) == 0);
	struct amdgpu_bo_info info;
	CHECK(amdgpu_bo_query_info(bo, &info) == 0 && info.alloc_size == 64 << 10 &&
	      info.preferred_heap == AMDGPU_GEM_DOMAIN_GTT && info.metadata.tiling_info == 0x55 &&
	      info.metadata.umd_metadata[0] == 0x12345678);
	uint32_t kms = 0;
	CHECK(amdgpu_bo_export(bo, amdgpu_bo_handle_type_kms, &kms) == 0 &&
	      kms == amdgpu_bo_get_handle(bo));
	uint32_t dmabuf = 0;
	CHECK(amdgpu_bo_export(bo, amdgpu_bo_handle_type_dma_buf_fd, &dmabuf) == 0);
	CHECK(fcntl((int)dmabuf, F_GETFD) & FD_CLOEXEC);
	struct amdgpu_bo_import_result imported;
	CHECK(amdgpu_bo_import(adev, amdgpu_bo_handle_type_dma_buf_fd, dmabuf, &imported) == 0);
	CHECK(imported.buf_handle == bo && imported.alloc_size == 64 << 10);
	close((int)dmabuf);
	CHECK(amdgpu_bo_free(imported.buf_handle) == 0);
	CHECK(amdgpu_bo_export(bo, amdgpu_bo_handle_type_gem_flink_name, &kms) == -EPERM);
	amdgpu_bo_handle userptr;
	CHECK(amdgpu_create_bo_from_user_mem(adev, cpu, 4096, &userptr) == -ENOSYS);

	/* The data, through the mmap wrapper and GEM_MMAP. */
	union drm_amdgpu_gem_mmap mm = { .in = { .handle = amdgpu_bo_get_handle(bo) } };
	CHECK(drmCommandWriteRead(fd, DRM_AMDGPU_GEM_MMAP, &mm, sizeof(mm)) == 0);
	uint32_t *map = drmFileMmap(NULL, 64 << 10, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				    (off_t)mm.out.addr_ptr);
	CHECK(map != MAP_FAILED && map[0] == 0x600df00d && map[(64 << 10) / 4 - 1] == 0xfeedface);
	CHECK(drmFileMunmap(map, 64 << 10) == 0);
	void *anon = drmFileMmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	CHECK(anon != MAP_FAILED && drmFileMunmap(anon, 16384) == 0);
	CHECK(amdgpu_bo_free(bo) == 0);

	/* A VRAM buffer, mapped through the aperture. */
	req = (struct amdgpu_bo_alloc_request){ .alloc_size = 256 << 10, .phys_alignment = 4096,
		.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM,
		.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED };
	CHECK(amdgpu_bo_alloc(adev, &req, &bo) == 0);
	CHECK(amdgpu_bo_cpu_map(bo, (void **)&cpu) == 0);
	memset(cpu, 0x5a, 256 << 10);
	CHECK(amdgpu_bo_cpu_unmap(bo) == 0 && amdgpu_bo_free(bo) == 0);

	/* Teardown: every file released once its descriptors are closed. */
	CHECK(amdgpu_device_deinitialize(adev) == 0);
	close(copy);
	CHECK(lx_loopback_open_files() == base + 1);
	close(fd);
	CHECK(files_reach(base));
	puts("PASS libdrm-mlg: device list and nodes, DRM files as descriptors (dup, release on "
	     "last close), version and caps, syncobj descriptors, sync_file wait/merge/accumulate, "
	     "amdgpu devices, VA ranges, GTT and VRAM buffers (map, metadata, dma-buf), mmap");
	return 0;
}
