/* The Linux-file RPC core (linuxu/src/amdgpu-rt/lx_files.c) with its frame
 * code (lx_frame.c) and ioctl tables (lx_describe.c), on the linuxu process
 * substrate, against a fixture render node and /dev/kfd whose
 * file_operations copy exactly as drm_ioctl, amdgpu_cs_ioctl and
 * drm_syncobj_wait_ioctl do. This test is the client: its own memory holds
 * the ioctl arguments, described, framed, run in the client's Linux process
 * and copied back.
 *
 * Covers: descriptor tables per client (isolation), nested pointers (CS
 * chunks and BO lists, INFO return pointers, version strings), undescribed
 * memory faulting, admission, async waits on workers (signal, timeout,
 * kill), absolute timeouts carried across clocks, argument pages shared by
 * concurrent calls, the async limit, PFN mmaps and their commit, and
 * teardown closing every file. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/chardev.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <uapi/linux/kfd_ioctl.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/amdgpu_drm.h>
#include <rt/lx_files.h>
#include <rt/lx_timing.h>

extern int usleep(unsigned int usec);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
	__FILE__, __LINE__, #c); abort(); } } while (0)

/* ---- the fixture devices ---- */

#define FIXTURE_BAR2	0xfc000000ULL
#define FIXTURE_BAR2_LEN (2ULL << 20)

static int opens, releases;

/* The device files the runtime adds (rt_lx_timing_register). */
static const struct device_attribute *timing_attr;
static struct device *timing_dev;
static int timing_registrations;
int device_create_file(struct device *dev, const struct device_attribute *attr)
{
	if (timing_attr && timing_dev == dev && attr == timing_attr)
		return -EEXIST;
	timing_dev = dev;
	timing_attr = attr;
	timing_registrations++;
	return 0;
}
static DECLARE_WAIT_QUEUE_HEAD(syncobj_wq);
static uint32_t signaled;	/* syncobj handles <= this are signaled */
static int waiters;

static int fx_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	filp->private_data = (void *)(uintptr_t)current->group_leader->pid;
	__atomic_add_fetch(&opens, 1, __ATOMIC_SEQ_CST);
	return 0;
}

static int fx_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	__atomic_add_fetch(&releases, 1, __ATOMIC_SEQ_CST);
	return 0;
}

static uint64_t fnv(uint64_t h, const void *bytes, size_t n)
{
	const uint8_t *p = bytes;

	for (size_t i = 0; i < n; ++i)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

/* drm_ioctl copies the block in, runs the handler and copies it out. */
static long fx_cs(union drm_amdgpu_cs *cs)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	uint64_t pointers[16];

	if (cs->in.num_chunks > 16)
		return -EINVAL;
	/* amdgpu_cs_pass1: the array of chunk pointers, each chunk, its data. */
	if (copy_from_user(pointers, u64_to_user_ptr(cs->in.chunks),
			   cs->in.num_chunks * sizeof(uint64_t)))
		return -EFAULT;
	for (uint32_t i = 0; i < cs->in.num_chunks; ++i) {
		struct drm_amdgpu_cs_chunk chunk;
		uint8_t data[256];

		if (copy_from_user(&chunk, u64_to_user_ptr(pointers[i]), sizeof(chunk)))
			return -EFAULT;
		if (chunk.length_dw * 4 > sizeof(data))
			return -EINVAL;
		if (copy_from_user(data, u64_to_user_ptr(chunk.chunk_data), chunk.length_dw * 4))
			return -EFAULT;
		h = fnv(h, data, chunk.length_dw * 4);
		if (chunk.chunk_id == AMDGPU_CHUNK_ID_BO_HANDLES) {
			struct drm_amdgpu_bo_list_in in;
			struct drm_amdgpu_bo_list_entry entries[64];

			memcpy(&in, data, sizeof(in));
			if (in.bo_number > 64 || in.bo_info_size != sizeof(entries[0]))
				return -EINVAL;
			if (copy_from_user(entries, u64_to_user_ptr(in.bo_info_ptr),
					   in.bo_number * sizeof(entries[0])))
				return -EFAULT;
			h = fnv(h, entries, in.bo_number * sizeof(entries[0]));
		}
	}
	cs->out.handle = h;
	return 0;
}

static long fx_syncobj_wait(struct drm_syncobj_wait *w)
{
	uint32_t handles[8];
	int64_t left;
	long r;

	if (!w->count_handles || w->count_handles > 8)
		return -EINVAL;
	if (copy_from_user(handles, u64_to_user_ptr(w->handles), w->count_handles * 4))
		return -EFAULT;
	/* drm_timeout_abs_to_jiffies: an absolute CLOCK_MONOTONIC deadline. */
	left = w->timeout_nsec - (int64_t)ktime_get_ns();
	if (left <= 0)
		return __atomic_load_n(&signaled, __ATOMIC_SEQ_CST) >= handles[0] ? 0 : -ETIME;
	__atomic_add_fetch(&waiters, 1, __ATOMIC_SEQ_CST);
	r = wait_event_interruptible_timeout(syncobj_wq,
		__atomic_load_n(&signaled, __ATOMIC_SEQ_CST) >= handles[0],
		nsecs_to_jiffies(left) + 1);
	__atomic_sub_fetch(&waiters, 1, __ATOMIC_SEQ_CST);
	if (r < 0)
		return r;
	if (!r)
		return -ETIME;
	w->first_signaled = 0;
	return 0;
}

static long fx_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	union {
		struct drm_version version;
		struct drm_amdgpu_info info;
		union drm_amdgpu_cs cs;
		struct drm_syncobj_wait wait;
		struct drm_gem_close close;
	} k;
	size_t size = _IOC_SIZE(cmd);
	long r;

	/* Every call runs in the process that opened the file. */
	CHECK((uintptr_t)filp->private_data == (uintptr_t)current->group_leader->pid);
	if (size > sizeof(k))
		return -EINVAL;
	if (copy_from_user(&k, (void __user *)arg, size))
		return -EFAULT;
	switch (cmd) {
	case DRM_IOCTL_VERSION: {
		static const char name[] = "fixture", date[] = "20260101", desc[] = "lx fixture";

		k.version.version_major = 3;
		k.version.version_minor = 64;
		if (k.version.name_len && copy_to_user(k.version.name, name,
				min_t(size_t, k.version.name_len, sizeof(name) - 1)))
			return -EFAULT;
		if (k.version.date_len && copy_to_user(k.version.date, date,
				min_t(size_t, k.version.date_len, sizeof(date) - 1)))
			return -EFAULT;
		if (k.version.desc_len && copy_to_user(k.version.desc, desc,
				min_t(size_t, k.version.desc_len, sizeof(desc) - 1)))
			return -EFAULT;
		k.version.name_len = sizeof(name) - 1;
		k.version.date_len = sizeof(date) - 1;
		k.version.desc_len = sizeof(desc) - 1;
		r = 0;
		break;
	}
	case DRM_IOCTL_AMDGPU_INFO: {
		uint8_t out[64];

		if (k.info.return_size > sizeof(out))
			return -EINVAL;
		for (uint32_t i = 0; i < k.info.return_size; ++i)
			out[i] = (uint8_t)(k.info.query + i);
		r = copy_to_user(u64_to_user_ptr(k.info.return_pointer), out, k.info.return_size) ?
			-EFAULT : 0;
		break;
	}
	case DRM_IOCTL_AMDGPU_CS:
		r = fx_cs(&k.cs);
		break;
	case DRM_IOCTL_SYNCOBJ_WAIT:
		r = fx_syncobj_wait(&k.wait);
		break;
	case DRM_IOCTL_GEM_CLOSE:
		r = k.close.handle == 7 ? 0 : -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	if (copy_to_user((void __user *)arg, &k, size))
		return -EFAULT;
	return r;
}

/* KFD's doorbell mmap: io_remap_pfn_range of the process's slice. */
static int fx_mmap(struct file *filp, struct vm_area_struct *vma)
{
	uint64_t bytes = vma->vm_end - vma->vm_start;
	uint64_t bus = FIXTURE_BAR2 + ((uint64_t)vma->vm_pgoff << PAGE_SHIFT);

	(void)filp;
	if (bytes > FIXTURE_BAR2_LEN - (bus - FIXTURE_BAR2))
		return -EINVAL;
	return io_remap_pfn_range(vma, vma->vm_start, bus >> PAGE_SHIFT, bytes,
				  vma->vm_page_prot);
}

static const struct file_operations fx_fops = {
	.open = fx_open,
	.release = fx_release,
	.unlocked_ioctl = fx_ioctl,
	.mmap = fx_mmap,
};

/* A mapping the fixture never sets up through GEM. */
int rt_lx_gem_map(struct drm_device *ddev, struct pci_dev *pdev,
		  struct vm_area_struct *vma, uint64_t length, void **pinned,
		  uint32_t *backing, uint32_t *cache,
		  int (*add)(void *arg, uint32_t bar, uint64_t addr, uint64_t bytes),
		  void *arg)
{
	(void)ddev; (void)pdev; (void)vma; (void)length; (void)pinned;
	(void)backing; (void)cache; (void)add; (void)arg;
	abort();
}
void rt_lx_gem_unpin(void *pinned)
{
	(void)pinned;
	abort();
}

/* ---- the client side ---- */

/* ioctl(2) as libmlg_drm issues it: describe, frame, call, copy back. */
static long client_ioctl(struct rt_lx_client *c, uint32_t dev, int fd, uint32_t cmd, void *arg)
{
	struct mlg_lx_span spans[MLG_LX_DESCRIBE_MAX];
	uint64_t timeout_va = 0, out_bytes = 0;
	uint32_t n = 0;
	static uint8_t frame[1 << 16], rep[1 << 16];
	size_t rep_bytes = 0;
	int64_t result = 0;
	long len;
	int r;

	r = mlg_lx_describe(dev, cmd, (uint64_t)(uintptr_t)arg, spans, MLG_LX_DESCRIBE_MAX, &n,
			    &timeout_va);
	if (r)
		return r;
	len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va,
			    ktime_get_ns(), frame, sizeof(frame), &out_bytes);
	if (len < 0)
		return len;
	r = rt_lx_ioctl(c, fd, cmd, frame, (size_t)len, rep, sizeof(rep), &rep_bytes, &result);
	if (r)
		return r;
	CHECK(rep_bytes == mlg_lx_reply_bytes(out_bytes));
	CHECK(!mlg_lx_apply_reply(frame, (size_t)len, rep, rep_bytes, NULL));
	return (long)result;
}

struct async_call {
	uint8_t frame[4096];
	long len;
	uint64_t token;
	int done;
	int keep;
	int64_t result;
	uint8_t inline_rep[256];
	size_t rep_bytes;
};

static int async_done(void *ctx, uint64_t token, int64_t result, const void *rbuf,
		      size_t reply_bytes)
{
	struct async_call *a = ctx;

	CHECK(token);
	a->result = result;
	a->rep_bytes = reply_bytes;
	if (!a->keep && reply_bytes <= sizeof(a->inline_rep))
		memcpy(a->inline_rep, rbuf, reply_bytes);
	__atomic_store_n(&a->done, 1, __ATOMIC_SEQ_CST);
	return !a->keep;
}

static int start_async(struct rt_lx_client *c, int fd, uint32_t cmd, void *arg,
		       struct async_call *a)
{
	struct mlg_lx_span spans[16];
	uint64_t timeout_va = 0;
	uint32_t n = 0;

	CHECK(mlg_lx_cmd_blocks(MLG_LX_DEV_RENDER, cmd));
	CHECK(!mlg_lx_describe(MLG_LX_DEV_RENDER, cmd, (uint64_t)(uintptr_t)arg, spans, 16, &n,
			       &timeout_va));
	a->len = mlg_lx_encode(cmd, (uint64_t)(uintptr_t)arg, spans, n, timeout_va,
			       ktime_get_ns(), a->frame, sizeof(a->frame), NULL);
	CHECK(a->len > 0);
	return rt_lx_ioctl_async(c, fd, cmd, a->frame, (size_t)a->len, async_done, a, &a->token);
}

static void wait_done(struct async_call *a)
{
	for (int i = 0; i < 5000 && !__atomic_load_n(&a->done, __ATOMIC_SEQ_CST); ++i)
		usleep(1000);
	CHECK(__atomic_load_n(&a->done, __ATOMIC_SEQ_CST));
}

static void wait_waiters(int count)
{
	for (int i = 0; i < 5000 && __atomic_load_n(&waiters, __ATOMIC_SEQ_CST) != count; ++i)
		usleep(1000);
	CHECK(__atomic_load_n(&waiters, __ATOMIC_SEQ_CST) == count);
}

static uint32_t seen_bar = 99;
static uint64_t seen_addr, seen_bytes;
static int collect_range(void *arg, uint32_t backing, uint32_t bar, uint64_t addr, uint64_t bytes)
{
	(void)arg;
	CHECK(backing == RT_LX_RANGE_BAR);
	seen_bar = bar;
	seen_addr = addr;
	seen_bytes = bytes;
	return 0;
}

static uint64_t ms_from_now(uint64_t ms)
{
	return ktime_get_ns() + ms * 1000000ULL;
}

int main(void)
{
	static struct drm_minor render = { .index = 128 };
	static struct drm_device ddev = { .render = &render };
	static struct pci_dev pdev;
	struct rt_lx_client *a = NULL, *b = NULL;
	int fd_a, fd_b, fd_kfd;

	CHECK(register_chrdev(DRM_MAJOR, "drm", &fx_fops) == 0);
	CHECK(register_chrdev(0, "kfd", &fx_fops) > 0);
	pdev.resource[2].start = FIXTURE_BAR2;
	pdev.resource[2].end = FIXTURE_BAR2 + FIXTURE_BAR2_LEN - 1;
	pdev.resource[2].flags = IORESOURCE_MEM;
	CHECK(rt_lx_client_create(&pdev, 0, "a", &a) == -ENODEV && !a);
	pci_set_drvdata(&pdev, &ddev);
	CHECK(!rt_lx_client_create(&pdev, 4242, "client-a", &a) && a);
	CHECK(rt_lx_client_pid(a) == 4242);
	CHECK(!rt_lx_client_create(&pdev, 0, "client-b", &b) && b);

	/* open(2): flags as Linux spells them; descriptors per process. */
	CHECK(rt_lx_open(a, MLG_LX_DEV_RENDER, 0) == -EINVAL);
	CHECK(rt_lx_open(a, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | 0x40 /* O_CREAT */) == -EINVAL);
	CHECK(rt_lx_open(a, 9, MLG_LX_O_RDWR) == -ENODEV);
	fd_a = rt_lx_open(a, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR | MLG_LX_O_CLOEXEC);
	fd_b = rt_lx_open(b, MLG_LX_DEV_RENDER, MLG_LX_O_RDWR);
	CHECK(fd_a == 0 && fd_b == 0 && opens == 2);
	fd_kfd = rt_lx_open(a, MLG_LX_DEV_KFD, MLG_LX_O_RDWR);
	CHECK(fd_kfd == 1 && rt_lx_open_files(a) == 2 && rt_lx_open_files(b) == 1);

	/* DRM_IOCTL_VERSION: three OUT strings behind the block. */
	char name[16] = {0}, date[16] = {0}, desc[4] = {0};
	struct drm_version version = {
		.name_len = sizeof(name) - 1, .name = name,
		.date_len = sizeof(date) - 1, .date = date,
		.desc_len = sizeof(desc), .desc = desc,
	};
	CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_VERSION, &version) == 0);
	CHECK(version.version_major == 3 && version.version_minor == 64);
	CHECK(!strcmp(name, "fixture") && !strcmp(date, "20260101") && !memcmp(desc, "lx f", 4));
	CHECK(version.name_len == 7 && version.desc_len == 10);

	/* AMDGPU_INFO: the result through return_pointer. */
	uint8_t info_out[24];
	memset(info_out, 0xee, sizeof(info_out));
	struct drm_amdgpu_info info = {
		.return_pointer = (uint64_t)(uintptr_t)info_out, .return_size = 20,
		.query = AMDGPU_INFO_DEV_INFO,
	};
	CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_AMDGPU_INFO, &info) == 0);
	for (int i = 0; i < 20; ++i)
		CHECK(info_out[i] == (uint8_t)(AMDGPU_INFO_DEV_INFO + i));
	CHECK(info_out[20] == 0xee);	/* beyond return_size: untouched */

	/* AMDGPU_CS: chunk pointers -> chunks -> data -> BO list, all nested. */
	struct drm_amdgpu_bo_list_entry bos[3] = { { 1, 0 }, { 2, 1 }, { 9, 2 } };
	struct drm_amdgpu_bo_list_in list_in = {
		.operation = ~0u, .list_handle = ~0u, .bo_number = 3,
		.bo_info_size = sizeof(bos[0]), .bo_info_ptr = (uint64_t)(uintptr_t)bos,
	};
	struct drm_amdgpu_cs_chunk_ib ib = { .ip_type = AMDGPU_HW_IP_COMPUTE, .va_start = 0x400000,
					     .ib_bytes = 64 };
	struct drm_amdgpu_cs_chunk chunks[2] = {
		{ AMDGPU_CHUNK_ID_BO_HANDLES, sizeof(list_in) / 4, (uint64_t)(uintptr_t)&list_in },
		{ AMDGPU_CHUNK_ID_IB, sizeof(ib) / 4, (uint64_t)(uintptr_t)&ib },
	};
	uint64_t chunk_ptrs[2] = { (uint64_t)(uintptr_t)&chunks[0], (uint64_t)(uintptr_t)&chunks[1] };
	union drm_amdgpu_cs cs = { .in = { .ctx_id = 1, .num_chunks = 2,
					   .chunks = (uint64_t)(uintptr_t)chunk_ptrs } };
	uint64_t expect = 0xcbf29ce484222325ULL;
	expect = fnv(expect, &list_in, sizeof(list_in));
	expect = fnv(expect, bos, sizeof(bos));
	expect = fnv(expect, &ib, sizeof(ib));
	CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_AMDGPU_CS, &cs) == 0);
	CHECK(cs.out.handle == expect);

	/* Memory the client did not describe faults like an unmapped page:
	 * a frame carrying only the argument block. */
	{
		union drm_amdgpu_cs cs2 = { .in = { .num_chunks = 2,
						    .chunks = (uint64_t)(uintptr_t)chunk_ptrs } };
		struct mlg_lx_span only = { (uint64_t)(uintptr_t)&cs2, sizeof(cs2), MLG_LX_SEG_INOUT };
		static uint8_t frame[4096], rep[4096];
		size_t rep_bytes;
		int64_t result = 0;
		long len = mlg_lx_encode(DRM_IOCTL_AMDGPU_CS, (uint64_t)(uintptr_t)&cs2, &only, 1, 0,
					 0, frame, sizeof(frame), NULL);
		CHECK(len > 0);
		CHECK(!rt_lx_ioctl(a, fd_a, DRM_IOCTL_AMDGPU_CS, frame, (size_t)len, rep, sizeof(rep),
				   &rep_bytes, &result));
		/* Unless the chunk array shares a page with the block: then the
		 * page is mapped but its other bytes are zero, and the fixture
		 * reads zero pointers. Either way nothing leaks through. */
		CHECK(result == -EFAULT);
		/* A wrong command scalar, a short reply buffer: not run. */
		CHECK(rt_lx_ioctl(a, fd_a, DRM_IOCTL_AMDGPU_INFO, frame, (size_t)len, rep, sizeof(rep),
				  &rep_bytes, &result) == -EINVAL);
		CHECK(rt_lx_ioctl(a, fd_a, DRM_IOCTL_AMDGPU_CS, frame, (size_t)len, rep, 8,
				  &rep_bytes, &result) == -ENOSPC);
	}

	/* Admission: a KFD command on the render node, a render command on
	 * KFD, unknown commands, bad descriptors. */
	{
		struct kfd_ioctl_get_version_args kv = {0};
		struct drm_gem_close gc = { .handle = 7 };

		CHECK(client_ioctl(a, MLG_LX_DEV_KFD, fd_a, AMDKFD_IOC_GET_VERSION, &kv) == -ENOTTY);
		CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_kfd, DRM_IOCTL_GEM_CLOSE, &gc) == -ENOTTY);
		CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
		CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, 17, DRM_IOCTL_GEM_CLOSE, &gc) == -EBADF);
		CHECK(mlg_lx_describe(MLG_LX_DEV_RENDER, DRM_IOCTL_MODE_GETRESOURCES, 1,
				      NULL, 0, &(uint32_t){0}, &(uint64_t){0}) == -ENOTTY);
		CHECK(!mlg_lx_cmd_known(MLG_LX_DEV_KFD, AMDKFD_IOC_SVM) &&
		      !mlg_lx_cmd_known(MLG_LX_DEV_KFD, AMDKFD_IOC_DBG_TRAP) &&
		      mlg_lx_cmd_known(MLG_LX_DEV_KFD, AMDKFD_IOC_CREATE_QUEUE));
		/* Client b's table has no descriptor 1; a's KFD file is a's. */
		CHECK(client_ioctl(b, MLG_LX_DEV_RENDER, 1, DRM_IOCTL_GEM_CLOSE, &gc) == -EBADF);
	}

	/* Async waits: a signal ends one; the result waits to be fetched. */
	{
		uint32_t handle = 5;
		struct drm_syncobj_wait w = { .handles = (uint64_t)(uintptr_t)&handle,
					      .timeout_nsec = (int64_t)ms_from_now(10000),
					      .count_handles = 1, .first_signaled = 77 };
		struct async_call *call = calloc(1, sizeof(*call));
		call->keep = 1;
		CHECK(!start_async(a, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &w, call));
		wait_waiters(1);
		CHECK(!call->done && rt_lx_async_outstanding(a) == 1);
		uint8_t rep[256];
		size_t rep_bytes = 0;
		int64_t result = 1;
		CHECK(rt_lx_result(a, call->token, rep, sizeof(rep), &rep_bytes, &result) == -EBUSY);
		__atomic_store_n(&signaled, 5, __ATOMIC_SEQ_CST);
		wake_up_all(&syncobj_wq);
		wait_done(call);
		CHECK(call->result == 0);
		CHECK(rt_lx_result(a, call->token, rep, 8, &rep_bytes, &result) == -ENOSPC);
		CHECK(!rt_lx_result(a, call->token, rep, sizeof(rep), &rep_bytes, &result));
		CHECK(result == 0 && rep_bytes == call->rep_bytes);
		CHECK(!mlg_lx_apply_reply(call->frame, (size_t)call->len, rep, rep_bytes, &result));
		CHECK(w.first_signaled == 0);
		CHECK(rt_lx_result(a, call->token, rep, sizeof(rep), &rep_bytes, &result) == -ENOENT);
		CHECK(rt_lx_async_outstanding(a) == 0);
		free(call);
	}

	/* The deadline crosses clocks as time left: 60 ms from now on ours
	 * expires about 60 ms later in the process. */
	{
		uint32_t handle = 50;
		struct drm_syncobj_wait w = { .handles = (uint64_t)(uintptr_t)&handle,
					      .timeout_nsec = (int64_t)ms_from_now(60),
					      .count_handles = 1 };
		struct async_call *call = calloc(1, sizeof(*call));
		uint64_t start = ktime_get_ns();
		CHECK(!start_async(a, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &w, call));
		wait_done(call);
		CHECK(call->result == -ETIME);
		CHECK(ktime_get_ns() - start >= 50000000ULL);
		/* Delivered inline: nothing kept. */
		CHECK(rt_lx_async_outstanding(a) == 0);
		CHECK(!mlg_lx_apply_reply(call->frame, (size_t)call->len, call->inline_rep,
					  call->rep_bytes, NULL));
		free(call);
		/* A zero deadline polls and travels unconverted. */
		w.timeout_nsec = 0;
		CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &w) == -ETIME);
	}

	/* Concurrent calls whose arguments share a page, and the limit on
	 * calls in flight. */
	{
		struct { uint32_t handle[MLG_LX_MAX_ASYNC + 1]; struct drm_syncobj_wait w[MLG_LX_MAX_ASYNC + 1]; } *s =
			calloc(1, sizeof(*s));
		struct async_call *calls = calloc(MLG_LX_MAX_ASYNC + 1, sizeof(*calls));
		for (unsigned i = 0; i <= MLG_LX_MAX_ASYNC; ++i) {
			s->handle[i] = 100 + i;
			s->w[i] = (struct drm_syncobj_wait){ .handles = (uint64_t)(uintptr_t)&s->handle[i],
				.timeout_nsec = (int64_t)ms_from_now(10000), .count_handles = 1,
				.first_signaled = 99 };
			calls[i].keep = 0;
		}
		for (unsigned i = 0; i < MLG_LX_MAX_ASYNC; ++i)
			CHECK(!start_async(a, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &s->w[i], &calls[i]));
		CHECK(start_async(a, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &s->w[MLG_LX_MAX_ASYNC],
				  &calls[MLG_LX_MAX_ASYNC]) == -EAGAIN);
		wait_waiters(MLG_LX_MAX_ASYNC);
		/* A synchronous call on the same pages while they wait. */
		struct drm_gem_close gc = { .handle = 7 };
		CHECK(client_ioctl(a, MLG_LX_DEV_RENDER, fd_a, DRM_IOCTL_GEM_CLOSE, &gc) == 0);
		__atomic_store_n(&signaled, 1000, __ATOMIC_SEQ_CST);
		wake_up_all(&syncobj_wq);
		for (unsigned i = 0; i < MLG_LX_MAX_ASYNC; ++i) {
			wait_done(&calls[i]);
			CHECK(calls[i].result == 0);
			CHECK(!mlg_lx_apply_reply(calls[i].frame, (size_t)calls[i].len, calls[i].inline_rep,
						  calls[i].rep_bytes, NULL));
			CHECK(s->w[i].first_signaled == 0);
		}
		CHECK(s->w[MLG_LX_MAX_ASYNC].first_signaled == 99);
		CHECK(rt_lx_async_outstanding(a) == 0);
		free(calls);
		free(s);
		__atomic_store_n(&signaled, 0, __ATOMIC_SEQ_CST);
	}

	/* mmap of a PFN range: a BAR range for the client, committed at the
	 * client's address, then unmapped. */
	{
		struct rt_lx_map_info map, map2, info2;
		CHECK(rt_lx_mmap(a, fd_a, 0, PAGE_SIZE, MLG_LX_PROT_READ, 0, &map) == -EINVAL);
		CHECK(rt_lx_mmap(a, fd_a, 1, PAGE_SIZE, MLG_LX_PROT_READ, MLG_LX_MAP_SHARED, &map) == -EINVAL);
		CHECK(rt_lx_mmap(a, 9, 0, PAGE_SIZE, MLG_LX_PROT_READ, MLG_LX_MAP_SHARED, &map) == -EBADF);
		CHECK(rt_lx_mmap(a, fd_a, 2 * PAGE_SIZE, 2 * PAGE_SIZE,
				 MLG_LX_PROT_READ | MLG_LX_PROT_WRITE, MLG_LX_MAP_SHARED, &map) == 0);
		CHECK(map.type >= MLG_LX_MMAP_TYPE_BASE && map.length == 2 * PAGE_SIZE &&
		      map.backing == RT_LX_RANGE_BAR && map.cache == RT_LX_CACHE_UNCACHED &&
		      map.ranges == 1);
		CHECK(!rt_lx_map_ranges(a, map.type, collect_range, NULL));
		CHECK(seen_bar == 2 && seen_addr == 2 * PAGE_SIZE && seen_bytes == 2 * PAGE_SIZE);
		CHECK(!rt_lx_map_cpu(a, map.type, 0, NULL));
		CHECK(rt_lx_map_info(b, map.type, &info2) == -ENOENT);	/* a's, not b's */
		const uint64_t va = 0x30000000000ULL;
		CHECK(!rt_lx_mmap_commit(a, map.type, va));
		CHECK(!rt_lx_mmap_commit(a, map.type, va));
		CHECK(rt_lx_mmap_commit(a, map.type, va + PAGE_SIZE) == -EBUSY);
		CHECK(!rt_lx_map_info(a, map.type, &info2) && info2.committed && info2.va == va);
		/* A second mapping cannot overlap the first. */
		CHECK(!rt_lx_mmap(a, fd_a, 0, PAGE_SIZE, MLG_LX_PROT_READ, MLG_LX_MAP_SHARED, &map2));
		CHECK(rt_lx_mmap_commit(a, map2.type, va + PAGE_SIZE) == -EBUSY);
		/* Arguments inside a mapping are not carried: the transport
		 * refuses rather than shadowing the mapped memory. */
		{
			struct mlg_lx_span in_map = { va + 64, 8, MLG_LX_SEG_IN };
			static uint8_t frame[4096], rep[4096];
			size_t rep_bytes;
			int64_t result;
			long len;

			/* The encoder reads the client's memory at va: point it at
			 * a real buffer by describing the span by hand. */
			struct mlg_lx_frame head = { .magic = MLG_LX_FRAME_MAGIC, .version = MLG_LX_VERSION,
				.header_bytes = sizeof(head), .cmd = DRM_IOCTL_GEM_CLOSE,
				.arg = in_map.va, .nsegs = 1 };
			struct mlg_lx_segment seg = { .va = in_map.va, .size = 8, .dir = MLG_LX_SEG_IN,
				.data_offset = sizeof(head) + sizeof(seg) };
			len = sizeof(head) + sizeof(seg) + 8;
			head.total_bytes = (uint32_t)len;
			memset(frame, 0, sizeof(frame));
			memcpy(frame, &head, sizeof(head));
			memcpy(frame + sizeof(head), &seg, sizeof(seg));
			CHECK(rt_lx_ioctl(a, fd_a, DRM_IOCTL_GEM_CLOSE, frame, (size_t)len, rep, sizeof(rep),
					  &rep_bytes, &result) == -EFAULT);
		}
		CHECK(rt_lx_mappings(a) == 2);
		CHECK(!rt_lx_munmap(a, map.type));
		CHECK(rt_lx_munmap(a, map.type) == -ENOENT);
		CHECK(!rt_lx_mmap_commit(a, map2.type, va + PAGE_SIZE));
		CHECK(rt_lx_mappings(a) == 1);	/* map2 stays for teardown */
	}

	/* close(2) runs the file's release once its last reference goes. */
	CHECK(rt_lx_close(a, fd_kfd) == 0 && releases == 1);
	CHECK(rt_lx_close(a, fd_kfd) == -EBADF);

	/* Teardown with a wait in flight: the kill ends it (EINTR), its
	 * callback runs before destroy returns, every file closes. */
	{
		uint32_t handle = 999;
		struct drm_syncobj_wait w = { .handles = (uint64_t)(uintptr_t)&handle,
					      .timeout_nsec = (int64_t)ms_from_now(60000),
					      .count_handles = 1 };
		struct async_call *call = calloc(1, sizeof(*call));
		call->keep = 1;	/* never fetched: dropped by the teardown */
		CHECK(!start_async(a, fd_a, DRM_IOCTL_SYNCOBJ_WAIT, &w, call));
		wait_waiters(1);
		rt_lx_client_destroy(a);
		CHECK(call->done && call->result == -EINTR);
		CHECK(releases == 2);	/* a's render file; b's stays open */
		free(call);
	}
	/* Hop timing: the device's mlg_lx_timing file, added once, shows the
	 * requests made above by number with each hop's mean. */
	{
		static char text[16384];
		char version[64];

		CHECK(timing_registrations == 1 && timing_dev == &pdev.dev && timing_attr &&
		      !strcmp(timing_attr->attr.name, "mlg_lx_timing"));
		CHECK(timing_attr->show(&pdev.dev, (struct device_attribute *)timing_attr, text) > 0);
		CHECK(strstr(text, "# primitives ns: clock="));
		snprintf(version, sizeof(version), "\n0x%02x ", (unsigned int)(DRM_IOCTL_VERSION & 0xff));
		const char *line = strstr(text, version);
		CHECK(line && strstr(line, " pages=") && strstr(line, " ioctl=") && strstr(line, " release="));
		CHECK(rt_lx_timing_show(text, 8) < 8);	/* bounded */
	}
	rt_lx_client_destroy(b);
	CHECK(releases == 3 && opens == 3);
	unregister_chrdev(DRM_MAJOR, "drm");
	puts("PASS lx files: per-client processes and descriptors, nested ioctl memory, faults, "
	     "admission, async waits (signal, timeout, kill), shared argument pages, mmap, teardown");
	return 0;
}
