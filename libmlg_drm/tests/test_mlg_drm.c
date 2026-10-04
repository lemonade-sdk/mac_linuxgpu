/* libmlg_drm on its own: Linux request numbers and structure sizes from
 * the uapi as the library builds it, the BSD-to-Linux request conversion,
 * errno translation, paths and flags, descriptor and mapping bookkeeping,
 * the frames it sends (nested memory of CS, BO lists and syncobj waits;
 * deadlines; the wait path), and how replies come back, against a
 * recording transport that checks every frame with the dext's checker. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mlg_drm.h"
#include "mlg_uapi.h"
#include <rt/lx_abi.h>

/* <sys/ioccom.h>'s encoding, spelled out: mlg_uapi.h replaces its macros
 * with the Linux ones in this file. */
#define BSD_IOC(inout, group, num, len) \
	((unsigned long)(inout) | ((unsigned long)((len) & IOCPARM_MASK) << 16) | \
	 ((unsigned long)(group) << 8) | (unsigned long)(num))

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

/* ---- a recording transport ---- */

static struct {
	int next_fd;
	uint32_t open_dev, open_flags;
	int closed;
	uint8_t frame[1 << 20];
	size_t frame_bytes;
	uint32_t cmd;
	int async;
	int64_t result;			/* what the next ioctl returns */
	uint8_t out_fill;		/* the bytes the "kernel" writes to OUT segments */
	int transport_error;		/* the call does not reach the driver */
	int mmaps, munmaps;
	uint64_t mmap_length, munmap_length;
	uint8_t page[16384];
} rec;

static int r_open(void *ctx, uint32_t dev, uint32_t flags)
{
	(void)ctx;
	rec.open_dev = dev;
	rec.open_flags = flags;
	return rec.next_fd++;
}

static int r_close(void *ctx, int fd)
{
	(void)ctx;
	rec.closed = fd;
	return fd >= 0 && fd < rec.next_fd ? 0 : -MLG_LX_EBADF;
}

static int r_ioctl(void *ctx, int fd, uint32_t cmd, const void *frame, size_t bytes, void *reply,
		   size_t cap, size_t *reply_bytes, int64_t *result, int async)
{
	struct mlg_lx_reply rh = { MLG_LX_REPLY_MAGIC, MLG_LX_VERSION, sizeof(rh), 0, 0, 0 };
	struct mlg_lx_frame h;
	uint64_t out = 0;

	(void)ctx;
	(void)fd;
	if (rec.transport_error)
		return rec.transport_error;
	/* What the dext would accept. */
	CHECK(!mlg_lx_frame_check(frame, bytes, cmd, &out));
	CHECK(bytes <= sizeof(rec.frame) && cap >= mlg_lx_reply_bytes(out));
	memcpy(rec.frame, frame, bytes);
	rec.frame_bytes = bytes;
	rec.cmd = cmd;
	rec.async = async;
	memcpy(&h, frame, sizeof(h));
	for (uint32_t i = 0; i < h.nsegs; ++i) {
		struct mlg_lx_segment s;

		memcpy(&s, (const uint8_t *)frame + sizeof(h) + i * sizeof(s), sizeof(s));
		rh.out_segments += !!(s.dir & MLG_LX_SEG_OUT);
	}
	rh.total_bytes = (uint32_t)mlg_lx_reply_bytes(out);
	rh.result = rec.result;
	memcpy(reply, &rh, sizeof(rh));
	memset((uint8_t *)reply + sizeof(rh), rec.out_fill, out);
	*reply_bytes = rh.total_bytes;
	*result = rec.result;
	return 0;
}

static int r_mmap(void *ctx, int fd, uint64_t offset, uint64_t length, uint32_t prot,
		  uint32_t flags, void **addr, uint64_t *handle)
{
	(void)ctx; (void)fd; (void)offset; (void)prot;
	CHECK(flags == MLG_LX_MAP_SHARED && length <= sizeof(rec.page));
	rec.mmap_length = length;
	*addr = rec.page;
	*handle = MLG_LX_MMAP_TYPE_BASE + (uint64_t)rec.mmaps++;
	return 0;
}

static int r_munmap(void *ctx, uint64_t handle, void *addr, uint64_t length)
{
	(void)ctx;
	CHECK(handle >= MLG_LX_MMAP_TYPE_BASE && addr == rec.page);
	rec.munmap_length = length;
	rec.munmaps++;
	return 0;
}

static int r_identity(void *ctx, struct mlg_pci_identity *out)
{
	(void)ctx;
	*out = (struct mlg_pci_identity){ .bus = 0xc3, .vendor_id = 0x1002, .device_id = 0x7551,
					  .subvendor_id = 0x1002, .subdevice_id = 0x0e3b,
					  .revision_id = 0xc0 };
	return 0;
}

static const struct mlg_transport recording = {
	.open = r_open, .close = r_close, .ioctl = r_ioctl, .mmap = r_mmap, .munmap = r_munmap,
	.identity = r_identity,
};

/* The frame's segments, for checks. */
static uint32_t segments(struct mlg_lx_segment *out, uint32_t cap)
{
	struct mlg_lx_frame h;

	memcpy(&h, rec.frame, sizeof(h));
	CHECK(h.nsegs <= cap);
	for (uint32_t i = 0; i < h.nsegs; ++i)
		memcpy(&out[i], rec.frame + sizeof(h) + i * sizeof(out[i]), sizeof(out[i]));
	return h.nsegs;
}

static int covered(const struct mlg_lx_segment *s, uint32_t n, const void *p, size_t bytes,
		   uint32_t dir)
{
	uint64_t a = (uint64_t)(uintptr_t)p;

	for (uint32_t i = 0; i < n; ++i)
		if (a >= s[i].va && a + bytes <= s[i].va + s[i].size && (s[i].dir & dir) == dir)
			return 1;
	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
	struct mlg_lx_segment seg[64];
	uint32_t n;
	int fd, kfd;

	/* Linux numbers, whatever the host: what a Linux libdrm sends. */
	CHECK(DRM_IOCTL_VERSION == 0xc0406400u);
	CHECK(DRM_IOCTL_AMDGPU_INFO == 0x40206445u);
	CHECK(DRM_IOCTL_AMDGPU_CS == 0xc0186444u);
	CHECK(DRM_IOCTL_AMDGPU_GEM_CREATE == 0xc0206440u);
	CHECK(DRM_IOCTL_SYNCOBJ_WAIT == 0xc02864c3u);
	CHECK(AMDKFD_IOC_GET_VERSION == 0x80084b01u);
	CHECK(sizeof(struct drm_version) == 64 && sizeof(union drm_amdgpu_cs) == 24 &&
	      sizeof(struct drm_amdgpu_info) == 32 && sizeof(struct drm_syncobj_wait) == 40);
	/* The same request as a macOS build of the header spells it. */
	CHECK(mlg_ioctl_from_bsd(BSD_IOC(IOC_INOUT, 'd', 0x00, sizeof(struct drm_version))) ==
	      DRM_IOCTL_VERSION);
	CHECK(mlg_ioctl_from_bsd(BSD_IOC(IOC_IN, 'd', 0x45, sizeof(struct drm_amdgpu_info))) ==
	      DRM_IOCTL_AMDGPU_INFO);
	CHECK(mlg_ioctl_from_bsd(BSD_IOC(IOC_OUT, 'K', 0x01,
					 sizeof(struct kfd_ioctl_get_version_args))) ==
	      AMDKFD_IOC_GET_VERSION);
	CHECK(mlg_ioctl_from_bsd(BSD_IOC(IOC_VOID, 'd', 0x10, 0)) == 0x6410u);

	/* Linux errnos in this platform's numbering. */
	CHECK(mlg_errno_from_linux(11) == EAGAIN && mlg_errno_from_linux(62) == ETIME &&
	      mlg_errno_from_linux(110) == ETIMEDOUT && mlg_errno_from_linux(95) == EOPNOTSUPP &&
	      mlg_errno_from_linux(22) == EINVAL && mlg_errno_from_linux(4) == EINTR &&
	      mlg_errno_from_linux(133) == EIO);

	CHECK(!mlg_drm_set_transport(&recording));

	/* open(2): paths and flags. */
	CHECK(mlg_open("/dev/dri/card64", O_RDWR) == -1 && errno == ENOENT);
	CHECK(mlg_open("/dev/dri/cardX", O_RDWR) == -1 && errno == ENOENT);
	CHECK(mlg_open("/dev/dri/renderD12", O_RDWR) == -1 && errno == ENOENT);
	fd = mlg_open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	CHECK(fd == 0 && rec.open_dev == MLG_LX_DEV_RENDER &&
	      rec.open_flags == (MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC | MLG_LX_O_NONBLOCK));
	kfd = mlg_open("/dev/kfd", O_RDWR);
	CHECK(kfd == 1 && rec.open_dev == MLG_LX_DEV_KFD && rec.open_flags == MLG_LX_O_RDWR);
	CHECK(mlg_drm_set_transport(&recording) == -1 && errno == EBUSY);

	/* An unknown descriptor, an unknown request, a request of the other
	 * device. */
	struct drm_amdgpu_info info = {0};
	CHECK(mlg_ioctl(7, DRM_IOCTL_AMDGPU_INFO, &info) == -1 && errno == EBADF);
	CHECK(mlg_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &info) == -1 && errno == ENOTTY);
	CHECK(mlg_ioctl(kfd, DRM_IOCTL_AMDGPU_INFO, &info) == -1 && errno == ENOTTY);

	/* AMDGPU_INFO: the block IN, the result OUT through return_pointer. */
	uint8_t result[40];
	memset(result, 0, sizeof(result));
	info = (struct drm_amdgpu_info){ .return_pointer = (uint64_t)(uintptr_t)result,
					 .return_size = sizeof(result), .query = AMDGPU_INFO_DEV_INFO };
	rec.out_fill = 0x5a;
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &info) == 0);
	n = segments(seg, 64);
	CHECK(rec.cmd == DRM_IOCTL_AMDGPU_INFO && !rec.async);
	CHECK(covered(seg, n, &info, sizeof(info), MLG_LX_SEG_IN));
	CHECK(covered(seg, n, result, sizeof(result), MLG_LX_SEG_OUT));
	CHECK(result[0] == 0x5a && result[39] == 0x5a);

	/* AMDGPU_CS: chunk pointers, chunks, chunk data and the BO list. */
	struct drm_amdgpu_bo_list_entry bos[4] = { {1, 0}, {2, 0}, {3, 0}, {4, 0} };
	struct drm_amdgpu_bo_list_in list = { .operation = ~0u, .list_handle = ~0u, .bo_number = 4,
		.bo_info_size = sizeof(bos[0]), .bo_info_ptr = (uint64_t)(uintptr_t)bos };
	struct drm_amdgpu_cs_chunk_ib ib = { .ip_type = AMDGPU_HW_IP_COMPUTE, .va_start = 1 << 22,
		.ib_bytes = 64 };
	struct drm_amdgpu_cs_chunk_sem sem = { 3 };
	struct drm_amdgpu_cs_chunk *chunks = calloc(3, sizeof(*chunks));
	uint64_t *ptrs = calloc(3, sizeof(*ptrs));
	chunks[0] = (struct drm_amdgpu_cs_chunk){ AMDGPU_CHUNK_ID_BO_HANDLES, sizeof(list) / 4,
		(uint64_t)(uintptr_t)&list };
	chunks[1] = (struct drm_amdgpu_cs_chunk){ AMDGPU_CHUNK_ID_IB, sizeof(ib) / 4,
		(uint64_t)(uintptr_t)&ib };
	chunks[2] = (struct drm_amdgpu_cs_chunk){ AMDGPU_CHUNK_ID_SYNCOBJ_OUT, sizeof(sem) / 4,
		(uint64_t)(uintptr_t)&sem };
	for (int i = 0; i < 3; ++i)
		ptrs[i] = (uint64_t)(uintptr_t)&chunks[i];
	union drm_amdgpu_cs cs = { .in = { .ctx_id = 1, .num_chunks = 3,
					   .chunks = (uint64_t)(uintptr_t)ptrs } };
	rec.out_fill = 0;
	CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_CS, &cs) == 0);
	n = segments(seg, 64);
	CHECK(covered(seg, n, &cs, sizeof(cs), MLG_LX_SEG_INOUT));
	CHECK(covered(seg, n, ptrs, 3 * sizeof(*ptrs), MLG_LX_SEG_IN));
	for (int i = 0; i < 3; ++i)
		CHECK(covered(seg, n, &chunks[i], sizeof(chunks[i]), MLG_LX_SEG_IN));
	CHECK(covered(seg, n, &list, sizeof(list), MLG_LX_SEG_IN));
	CHECK(covered(seg, n, bos, sizeof(bos), MLG_LX_SEG_IN));
	CHECK(covered(seg, n, &ib, sizeof(ib), MLG_LX_SEG_IN));
	CHECK(covered(seg, n, &sem, sizeof(sem), MLG_LX_SEG_IN));
	CHECK(!covered(seg, n, (void *)(uintptr_t)ib.va_start, 4, MLG_LX_SEG_IN));	/* a GPU VA */
	free(chunks);
	free(ptrs);

	/* A wait: async, its handle array IN, the deadline as time left. */
	uint32_t handles[2] = { 5, 6 };
	struct drm_syncobj_wait wait = { .handles = (uint64_t)(uintptr_t)handles,
		.timeout_nsec = (int64_t)(now_ns() + 3000000000ull), .count_handles = 2 };
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0);
	{
		struct mlg_lx_frame h;

		memcpy(&h, rec.frame, sizeof(h));
		n = segments(seg, 64);
		CHECK(rec.async && covered(seg, n, handles, sizeof(handles), MLG_LX_SEG_IN));
		CHECK((h.flags & MLG_LX_FRAME_TIMEOUT) &&
		      h.timeout_va == (uint64_t)(uintptr_t)&wait.timeout_nsec);
		CHECK(h.timeout_ns > 2000000000ull && h.timeout_ns <= 3000000000ull);
	}
	/* A zero deadline polls: no worker, nothing to convert. */
	wait.timeout_nsec = 0;
	CHECK(mlg_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == 0 && !rec.async);
	{
		struct mlg_lx_frame h;

		memcpy(&h, rec.frame, sizeof(h));
		CHECK(!h.flags);
	}

	/* DRM_IOCTL_VERSION with no string buffers asks for lengths only. */
	struct drm_version version = {0};
	CHECK(mlg_ioctl(fd, DRM_IOCTL_VERSION, &version) == 0);
	CHECK(segments(seg, 64) == 1);

	/* The driver's errno, and a transport failure. */
	rec.result = -62;	/* ETIME */
	CHECK(mlg_ioctl(fd, DRM_IOCTL_VERSION, &version) == -1 && errno == ETIME &&
	      mlg_last_linux_errno() == 62);
	rec.result = 0;
	rec.transport_error = -MLG_LX_ENODEV;
	CHECK(mlg_ioctl(fd, DRM_IOCTL_VERSION, &version) == -1 && errno == ENODEV);
	rec.transport_error = 0;

	/* A large BO list goes in one frame beyond the inline sizes. */
	{
		struct drm_amdgpu_bo_list_entry *many = calloc(20000, sizeof(*many));
		union drm_amdgpu_bo_list bl = { .in = { .operation = AMDGPU_BO_LIST_OP_CREATE,
			.bo_number = 20000, .bo_info_size = sizeof(*many),
			.bo_info_ptr = (uint64_t)(uintptr_t)many } };

		CHECK(mlg_ioctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &bl) == 0);
		CHECK(rec.frame_bytes > 160000);
		free(many);
	}

	/* KFD: an array the kernel writes. */
	struct kfd_process_device_apertures ap[4];
	struct kfd_ioctl_get_process_apertures_new_args apn = {
		.kfd_process_device_apertures_ptr = (uint64_t)(uintptr_t)ap, .num_of_nodes = 4 };
	CHECK(mlg_ioctl(kfd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW, &apn) == 0);
	n = segments(seg, 64);
	CHECK(covered(seg, n, ap, sizeof(ap), MLG_LX_SEG_OUT));
	CHECK(mlg_ioctl(kfd, AMDKFD_IOC_SVM, &apn) == -1 && errno == ENOTTY);

	/* mmap(2)/munmap(2): shared, whole mappings, no fixed placement. */
	void *p = mlg_mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1 << 20);
	CHECK(p == rec.page && rec.mmaps == 1);
	CHECK(mlg_mmap(NULL, 16384, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mlg_mmap(rec.page, 16384, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED);
	CHECK(mlg_mmap(NULL, 16384, PROT_READ, MAP_SHARED, 9, 0) == MAP_FAILED && errno == EBADF);
	CHECK(mlg_munmap(p, 4096) == -1 && errno == EINVAL);
	CHECK(mlg_is_mapping(p, 16384) && !mlg_is_mapping(p, 4096));
	CHECK(mlg_munmap(p, 16384) == 0 && rec.munmaps == 1);
	CHECK(mlg_munmap(p, 16384) == -1 && !mlg_is_mapping(p, 16384));
	/* A length short of a page maps the whole page, as mmap(2) does;
	 * munmap takes the caller's length and unmaps the page. */
	p = mlg_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1 << 20);
	CHECK(p == rec.page && rec.mmap_length == (uint64_t)getpagesize());
	CHECK(mlg_munmap(p, 4096) == 0 && rec.munmap_length == (uint64_t)getpagesize());

	/* A request encoded with more directions than the driver's table
	 * declares (Mesa issues GEM_VA read-write; the table says write)
	 * goes out in the table's encoding, as the DRM core would take it. */
	struct drm_amdgpu_gem_va va_rw = { .handle = 1, .operation = AMDGPU_VA_OP_MAP,
		.va_address = 1 << 21, .map_size = 1 << 16 };
	const unsigned long gem_va_rw = (DRM_IOCTL_AMDGPU_GEM_VA & ~(3ul << 30)) | (3ul << 30);
	rec.result = 0;
	CHECK(mlg_ioctl(fd, gem_va_rw, &va_rw) == 0 && rec.cmd == DRM_IOCTL_AMDGPU_GEM_VA);
	/* KFD requests are known by range: no such rewriting there. */
	const unsigned long svm_read = (AMDKFD_IOC_SVM & ~(3ul << 30)) | (2ul << 30);
	CHECK(mlg_ioctl(kfd, svm_read, &apn) == -1 && errno == ENOTTY);

	/* The GPU's PCI identity: the transport's when it has one... */
	struct mlg_pci_identity id;
	CHECK(mlg_pci_identity(&id) == 0 && id.vendor_id == 0x1002 && id.device_id == 0x7551 &&
	      id.bus == 0xc3 && id.subvendor_id == 0x1002);

	/* The primary node: KMS queries with their arrays OUT, framebuffer
	 * requests; nothing that changes the display. */
	{
		int card = mlg_open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		uint32_t crtcs[4], connectors[3];
		struct drm_mode_card_res res = {
			.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs, .count_crtcs = 4,
			.connector_id_ptr = (uint64_t)(uintptr_t)connectors, .count_connectors = 3,
		};
		struct drm_mode_crtc set = { 0 };
		struct mlg_lx_segment seg[8];
		uint32_t n;

		CHECK(card == 2 && rec.open_dev == MLG_LX_DEV_PRIMARY);
		rec.result = 0;
		CHECK(mlg_ioctl(card, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 &&
		      rec.cmd == DRM_IOCTL_MODE_GETRESOURCES);
		n = segments(seg, 8);
		CHECK(n == 3);
		for (uint32_t i = 0; i < n; i++) {
			if (seg[i].va == (uint64_t)(uintptr_t)crtcs)
				CHECK(seg[i].size == sizeof(crtcs) && seg[i].dir == MLG_LX_SEG_OUT);
			else if (seg[i].va == (uint64_t)(uintptr_t)connectors)
				CHECK(seg[i].size == sizeof(connectors) && seg[i].dir == MLG_LX_SEG_OUT);
			else
				CHECK(seg[i].va == (uint64_t)(uintptr_t)&res && seg[i].dir == MLG_LX_SEG_INOUT);
		}
		CHECK(mlg_ioctl(card, DRM_IOCTL_MODE_SETCRTC, &set) == -1 && errno == ENOTTY);
		CHECK(mlg_ioctl(card, DRM_IOCTL_AMDGPU_INFO, &info) == -1 && errno == ENOTTY);
		/* LX_SCANOUT needs a transport that carries it. */
		struct mlg_lx_scanout req = { .version = MLG_LX_SCANOUT_VERSION,
					      .op = MLG_LX_SCANOUT_PRESENT, .fd = fd };
		struct mlg_lx_scanout_state st;
		CHECK(mlg_scanout(&req, &st) == -1 && errno == EBADF);	/* a render file */
		req.fd = card;
		CHECK(mlg_scanout(&req, &st) == -1 && errno == ENODEV);
		CHECK(mlg_close(card) == 0);
	}

	CHECK(mlg_close(kfd) == 0 && rec.closed == kfd);
	CHECK(mlg_ioctl(kfd, AMDKFD_IOC_GET_VERSION, &apn) == -1 && errno == EBADF);
	CHECK(mlg_close(fd) == 0);
	/* ... else what AMDGPU_INFO_DEV_INFO reports, on a render node it
	 * opens and closes. */
	struct mlg_transport plain = recording;
	plain.identity = NULL;
	CHECK(mlg_drm_set_transport(&plain) == 0);
	rec.result = 0;
	rec.out_fill = 0x51;
	CHECK(mlg_pci_identity(&id) == 0 && id.vendor_id == 0x1002 && id.device_id == 0x5151 &&
	      id.revision_id == 0x51 && id.bus == 0 && rec.open_dev == MLG_LX_DEV_RENDER);
	CHECK(rec.cmd == DRM_IOCTL_AMDGPU_INFO && rec.closed == rec.next_fd - 1);
	CHECK(mlg_drm_set_transport(NULL) == 0);
	puts("PASS libmlg_drm: Linux request numbers and structures, BSD conversion, errnos, "
	     "paths and flags, nested request memory, waits and deadlines, replies, mmap, "
	     "table encodings, PCI identity");
	return 0;
}
