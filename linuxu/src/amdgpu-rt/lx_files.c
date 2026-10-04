/* Linux-file clients (rt/lx_files.h): a linuxu process per macOS client and
 * the system calls it makes on character devices of the GPU. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <rt/chrdev.h>
#include <rt/lx_files.h>
#include <rt/process.h>
#include "lx_internal.h"

#ifndef ERESTARTSYS
#define ERESTARTSYS		512
#endif
#define LX_ERESTART_LAST	516	/* ERESTART_RESTARTBLOCK */

/* Where a VMA waits between rt_lx_mmap and its commit: a window of the
 * process address space no client address reaches (MLG_LX_VA_LIMIT and
 * up, below TASK_SIZE_MAX). */
#define LX_PENDING_VA		MLG_LX_VA_LIMIT

/* One page of call arguments mapped in the process. Shared by every call
 * whose segments touch it while any of them runs. */
struct lx_page {
	struct vm_area_struct *vma;
	unsigned int refs;
	void *buf;
};

struct lx_range {
	uint32_t bar;
	uint64_t addr, bytes;
};

struct lx_map {
	struct lx_map *next;
	uint64_t type;
	struct vm_area_struct *vma;	/* owns the file reference */
	void *pinned;			/* the GEM buffer pinned while mapped */
	uint64_t length;
	uint32_t backing, cache;
	struct lx_range *ranges;
	uint32_t nranges;
	bool committed;
};

struct lx_async {
	struct lx_async *next;
	struct rt_lx_client *c;
	uint64_t token;
	int fd;
	uint32_t cmd;
	void *frame;
	size_t frame_bytes;
	rt_lx_done_fn done;
	void *ctx;
	pthread_t thread;
	bool started, finished, joined;
	int64_t result;
	void *rep;
	size_t reply_bytes;
	int status;	/* 0, or why the call did not run */
};

struct rt_lx_client {
	struct pci_dev *pdev;
	struct drm_device *ddev;
	struct linuxu_process *proc;
	unsigned int kfd_major;
	bool have_kfd;
	pthread_mutex_t lock;		/* state, async list, maps */
	pthread_cond_t changed;
	pthread_mutex_t arena;		/* lx_page reference counts */
	bool dying;
	unsigned int calls;		/* calls inside the process */
	struct lx_async *async;
	unsigned int async_count;
	uint64_t next_token;
	struct lx_map *maps;
	uint64_t next_map;
	uint64_t pending_va;
	const struct rt_lx_display_hooks *display;	/* primary node and LX_SCANOUT */
};

/* ---- client ---- */

int rt_lx_client_create(struct pci_dev *pdev, int pid, const char *comm,
			struct rt_lx_client **out)
{
	struct drm_device *ddev = pdev ? pci_get_drvdata(pdev) : NULL;
	struct rt_lx_client *c;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!ddev || !ddev->render)
		return -ENODEV;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->pdev = pdev;
	c->ddev = ddev;
	c->next_token = 1;
	c->pending_va = LX_PENDING_VA;
	pthread_mutex_init(&c->lock, NULL);
	pthread_cond_init(&c->changed, NULL);
	pthread_mutex_init(&c->arena, NULL);
	c->proc = linuxu_process_create(pid, comm && comm[0] ? comm : "lx-client");
	if (!c->proc) {
		pthread_mutex_destroy(&c->arena);
		pthread_cond_destroy(&c->changed);
		pthread_mutex_destroy(&c->lock);
		kfree(c);
		return -ENOMEM;
	}
	*out = c;
	return 0;
}

int rt_lx_client_pid(const struct rt_lx_client *c)
{
	return c ? linuxu_process_leader(c->proc)->pid : 0;
}

/* Admit one call into the process; -ESRCH once the client is going away. */
static int call_begin(struct rt_lx_client *c)
{
	pthread_mutex_lock(&c->lock);
	if (c->dying) {
		pthread_mutex_unlock(&c->lock);
		return -ESRCH;
	}
	c->calls++;
	pthread_mutex_unlock(&c->lock);
	return 0;
}

static void call_end(struct rt_lx_client *c)
{
	pthread_mutex_lock(&c->lock);
	c->calls--;
	pthread_cond_broadcast(&c->changed);
	pthread_mutex_unlock(&c->lock);
}

/* What a descriptor of the process is: MLG_LX_DEV_* or 0. Caller is
 * inside the process. */
static uint32_t file_device(struct rt_lx_client *c, struct file *file)
{
	struct inode *inode = file ? file->f_inode : NULL;

	if (!inode || !inode->i_rdev)
		return 0;
	if (MAJOR(inode->i_rdev) == DRM_MAJOR &&
	    MINOR(inode->i_rdev) == (unsigned int)c->ddev->render->index)
		return MLG_LX_DEV_RENDER;
	if (MAJOR(inode->i_rdev) == DRM_MAJOR && c->ddev->primary &&
	    MINOR(inode->i_rdev) == (unsigned int)c->ddev->primary->index)
		return MLG_LX_DEV_PRIMARY;
	if (c->have_kfd && MAJOR(inode->i_rdev) == c->kfd_major)
		return MLG_LX_DEV_KFD;
	return 0;
}

static long lx_errno(long r)
{
	/* The system call layer turns restart requests into EINTR when the
	 * call is not restarted; the client restarts it. */
	return r <= -ERESTARTSYS && r >= -LX_ERESTART_LAST ? -EINTR : r;
}

int rt_lx_open(struct rt_lx_client *c, uint32_t dev, uint32_t flags)
{
	struct linuxu_process_saved saved;
	struct file *file;
	dev_t devt;
	int fd, r;

	if (!c)
		return -EINVAL;
	if ((flags & MLG_LX_O_ACCMODE) != MLG_LX_O_RDWR ||
	    (flags & ~(MLG_LX_O_ACCMODE | MLG_LX_O_NONBLOCK | MLG_LX_O_CLOEXEC)))
		return -EINVAL;
	switch (dev) {
	case MLG_LX_DEV_RENDER:
		devt = MKDEV(DRM_MAJOR, c->ddev->render->index);
		break;
	case MLG_LX_DEV_PRIMARY:
		/* Only with the display's hooks, which keep the file from
		 * being DRM master. */
		if (!c->display || !c->ddev->primary)
			return -ENODEV;
		devt = MKDEV(DRM_MAJOR, c->ddev->primary->index);
		break;
	case MLG_LX_DEV_KFD: {
		unsigned int major;

		/* KFD registers its device once it initialized. */
		if (linuxu_chrdev_find("kfd", &major))
			return -ENODEV;
		pthread_mutex_lock(&c->lock);
		c->kfd_major = major;
		c->have_kfd = true;
		pthread_mutex_unlock(&c->lock);
		devt = MKDEV(major, 0);
		break;
	}
	default:
		return -ENODEV;
	}
	r = call_begin(c);
	if (r)
		return r;
	r = linuxu_process_enter(c->proc, &saved);
	if (!r) {
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			r = fd;
		} else {
			if (dev == MLG_LX_DEV_PRIMARY)
				c->display->primary_lock();
			r = linuxu_chrdev_open(devt, O_RDWR | O_CLOEXEC |
					       ((flags & MLG_LX_O_NONBLOCK) ? O_NONBLOCK : 0),
					       &file);
			/* drm_open made the first opener master; give it up
			 * before anything else can see it. */
			if (!r && dev == MLG_LX_DEV_PRIMARY) {
				int dropped = c->display->primary_opened(c->ddev, file);

				if (dropped) {
					fput(file);
					r = dropped;
				}
			}
			if (dev == MLG_LX_DEV_PRIMARY)
				c->display->primary_unlock();
			if (r) {
				put_unused_fd(fd);
			} else {
				fd_install(fd, file);
				r = fd;
			}
		}
		linuxu_process_leave(&saved);
	}
	call_end(c);
	return (int)lx_errno(r);
}

int rt_lx_close(struct rt_lx_client *c, int fd)
{
	struct linuxu_process_saved saved;
	int r;

	if (!c || fd < 0)
		return -EBADF;
	r = call_begin(c);
	if (r)
		return r;
	r = linuxu_process_enter(c->proc, &saved);
	if (!r) {
		r = close_fd(fd);
		linuxu_process_leave(&saved);
	}
	call_end(c);
	return r;
}

unsigned int rt_lx_open_files(struct rt_lx_client *c)
{
	return c ? linuxu_files_count(linuxu_process_files(c->proc)) : 0;
}

void rt_lx_client_set_display(struct rt_lx_client *c, const struct rt_lx_display_hooks *hooks)
{
	if (c)
		c->display = hooks;
}

int rt_lx_scanout(struct rt_lx_client *c, const struct mlg_lx_scanout *req,
		  struct mlg_lx_scanout_state *state)
{
	struct linuxu_process_saved saved;
	struct mlg_lx_scanout copy;
	struct file *file = NULL;
	int r;

	if (!c || !req || !state)
		return -EINVAL;
	memset(state, 0, sizeof(*state));
	state->version = MLG_LX_SCANOUT_VERSION;
	/* The request may live in memory the client can still write. */
	memcpy(&copy, req, sizeof(copy));
	if (copy.version != MLG_LX_SCANOUT_VERSION || copy.reserved[0] || copy.reserved[1] ||
	    !memchr(copy.connector, 0, sizeof(copy.connector)))
		return -EINVAL;
	if (!c->display)
		return -ENODEV;
	r = call_begin(c);
	if (r)
		return r;
	r = linuxu_process_enter(c->proc, &saved);
	if (r) {
		call_end(c);
		return r;
	}
	/* Framebuffers and syncobjs are named in the client's primary file. */
	if (copy.op == MLG_LX_SCANOUT_TEST || copy.op == MLG_LX_SCANOUT_PRESENT) {
		file = fget(copy.fd);
		if (!file)
			r = -EBADF;
		else if (file_device(c, file) != MLG_LX_DEV_PRIMARY)
			r = -EINVAL;
	}
	if (!r)
		r = c->display->scanout(c->pdev, c, file, &copy, state);
	if (file)
		fput(file);
	linuxu_process_leave(&saved);
	call_end(c);
	return (int)lx_errno(r);
}

/* ---- argument pages ---- */

static void lx_page_close(struct vm_area_struct *vma)
{
	/* Only an exiting mm closes a page a call still holds; the call's
	 * own release never runs this. */
	struct lx_page *page = vma->vm_private_data;

	if (page) {
		kfree(page->buf);
		kfree(page);
		vma->vm_private_data = NULL;
	}
}

static const struct vm_operations_struct lx_page_ops = {
	.close = lx_page_close,
};

struct lx_call_pages {
	uint64_t *va;		/* sorted page addresses */
	struct lx_page **page;
	uint32_t count;
};

static void pages_release(struct rt_lx_client *c, struct lx_call_pages *p)
{
	struct mm_struct *mm = linuxu_process_mm(c->proc);

	pthread_mutex_lock(&c->arena);
	for (uint32_t i = 0; i < p->count; ++i) {
		struct lx_page *page = p->page[i];

		if (!page || --page->refs)
			continue;
		mmap_write_lock(mm);
		linuxu_mm_remove_vma(mm, page->vma);
		mmap_write_unlock(mm);
		vm_area_free(page->vma);
		kfree(page->buf);
		kfree(page);
	}
	pthread_mutex_unlock(&c->arena);
	kfree(p->va);
	kfree(p->page);
	p->va = NULL;
	p->page = NULL;
	p->count = 0;
}

/* Map (or share) every page the segments touch. -EFAULT when a page
 * belongs to something else in the address space (a committed mmap). */
static int pages_acquire(struct rt_lx_client *c, const struct mlg_lx_segment *segs,
			 uint32_t nsegs, struct lx_call_pages *p)
{
	struct mm_struct *mm = linuxu_process_mm(c->proc);
	uint64_t total = 0;
	uint32_t n = 0;
	int r = 0;

	memset(p, 0, sizeof(*p));
	for (uint32_t i = 0; i < nsegs; ++i) {
		uint64_t first = segs[i].va & PAGE_MASK;
		uint64_t last = (segs[i].va + segs[i].size - 1) & PAGE_MASK;

		total += ((last - first) >> PAGE_SHIFT) + 1;
	}
	if (total > MLG_LX_MAX_ARG_PAGES)
		return -E2BIG;
	if (!total)
		return 0;
	p->va = kcalloc(total, sizeof(*p->va), GFP_KERNEL);
	p->page = kcalloc(total, sizeof(*p->page), GFP_KERNEL);
	if (!p->va || !p->page) {
		kfree(p->va);
		kfree(p->page);
		p->va = NULL;
		p->page = NULL;
		return -ENOMEM;
	}
	/* Segments are sorted and disjoint, so their pages come in order;
	 * only neighbours can repeat a page. */
	for (uint32_t i = 0; i < nsegs; ++i) {
		uint64_t first = segs[i].va & PAGE_MASK;
		uint64_t last = (segs[i].va + segs[i].size - 1) & PAGE_MASK;

		for (uint64_t va = first; va <= last; va += PAGE_SIZE)
			if (!n || p->va[n - 1] != va)
				p->va[n++] = va;
	}
	p->count = n;
	pthread_mutex_lock(&c->arena);
	for (uint32_t i = 0; i < n && !r; ++i) {
		struct vm_area_struct *vma;
		struct lx_page *page;

		mmap_write_lock(mm);
		vma = linuxu_find_vma_locked(mm, p->va[i]);
		if (vma && vma->vm_start <= p->va[i]) {
			if (vma->vm_ops == &lx_page_ops && vma->vm_private_data) {
				page = vma->vm_private_data;
				page->refs++;
				p->page[i] = page;
			} else {
				r = -EFAULT;
			}
			mmap_write_unlock(mm);
			continue;
		}
		page = kzalloc(sizeof(*page), GFP_KERNEL);
		vma = vm_area_alloc(mm);
		if (page)
			page->buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!page || !vma || !page->buf) {
			if (page)
				kfree(page->buf);
			kfree(page);
			vm_area_free(vma);
			mmap_write_unlock(mm);
			r = -ENOMEM;
			continue;
		}
		vma->vm_start = p->va[i];
		vma->vm_end = p->va[i] + PAGE_SIZE;
		vma->vm_flags = VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE;
		vma->vm_ops = &lx_page_ops;
		vma->vm_private_data = page;
		vma->linuxu_kaddr = page->buf;
		page->vma = vma;
		page->refs = 1;
		r = linuxu_mm_insert_vma(mm, vma);
		mmap_write_unlock(mm);
		if (r) {
			vm_area_free(vma);
			kfree(page->buf);
			kfree(page);
			r = r == -EEXIST ? -EFAULT : r;
			continue;
		}
		p->page[i] = page;
	}
	pthread_mutex_unlock(&c->arena);
	if (r)
		pages_release(c, p);
	return r;
}

static uint8_t *page_byte(struct lx_call_pages *p, uint64_t va, uint64_t *avail)
{
	uint64_t base = va & PAGE_MASK;
	uint32_t lo = 0, hi = p->count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (p->va[mid] < base)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= p->count || p->va[lo] != base)
		return NULL;
	*avail = PAGE_SIZE - (va - base);
	return (uint8_t *)p->page[lo]->buf + (va - base);
}

static void pages_copy(struct lx_call_pages *p, uint64_t va, void *bytes, uint64_t size,
		       bool to_pages)
{
	while (size) {
		uint64_t avail = 0, n;
		uint8_t *at = page_byte(p, va, &avail);

		if (!at)
			return;	/* cannot happen: every segment page is mapped */
		n = size < avail ? size : avail;
		if (to_pages)
			memcpy(at, bytes, n);
		else
			memcpy(bytes, at, n);
		va += n;
		bytes = (uint8_t *)bytes + n;
		size -= n;
	}
}

/* ---- ioctl ---- */

/* Run one validated frame. Caller holds a call reference. */
static int run_ioctl(struct rt_lx_client *c, int fd, uint32_t cmd, const void *frame,
		     size_t frame_bytes, uint64_t out_bytes, void *rep, size_t *reply_bytes,
		     int64_t *result)
{
	const struct mlg_lx_frame *head = frame;
	const struct mlg_lx_segment *segs =
		(const struct mlg_lx_segment *)((const uint8_t *)frame + sizeof(*head));
	struct linuxu_process_saved saved;
	struct lx_call_pages pages;
	struct mlg_lx_reply rh;
	struct file *file;
	uint8_t *out;
	long ret;
	int r;

	(void)frame_bytes;
	r = pages_acquire(c, segs, head->nsegs, &pages);
	if (r)
		return r;
	for (uint32_t i = 0; i < head->nsegs; ++i)
		if (segs[i].dir & MLG_LX_SEG_IN)
			pages_copy(&pages, segs[i].va,
				   (uint8_t *)frame + segs[i].data_offset, segs[i].size, true);
	if (head->flags & MLG_LX_FRAME_TIMEOUT) {
		uint64_t now = ktime_get_ns(), deadline;

		deadline = head->timeout_ns > (uint64_t)INT64_MAX - now ?
			(uint64_t)INT64_MAX : now + head->timeout_ns;
		pages_copy(&pages, head->timeout_va, &deadline, sizeof(deadline), true);
	}
	r = linuxu_process_enter(c->proc, &saved);
	if (r) {
		pages_release(c, &pages);
		return r;
	}
	file = fget(fd);
	if (!file) {
		ret = -EBADF;
	} else {
		uint32_t dev = file_device(c, file);

		if (!dev || !mlg_lx_cmd_known(dev, cmd))
			ret = -ENOTTY;
		else if (!file->f_op || !file->f_op->unlocked_ioctl)
			ret = -ENOTTY;
		else
			ret = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)head->arg);
		fput(file);
	}
	linuxu_process_leave(&saved);
	ret = lx_errno(ret);

	/* Linux leaves whatever the call wrote in user memory, failed or
	 * not: every OUT segment goes back. */
	memset(&rh, 0, sizeof(rh));
	rh.magic = MLG_LX_REPLY_MAGIC;
	rh.version = MLG_LX_VERSION;
	rh.header_bytes = sizeof(rh);
	rh.total_bytes = (uint32_t)mlg_lx_reply_bytes(out_bytes);
	rh.result = ret;
	out = (uint8_t *)rep + sizeof(rh);
	for (uint32_t i = 0; i < head->nsegs; ++i) {
		if (!(segs[i].dir & MLG_LX_SEG_OUT))
			continue;
		pages_copy(&pages, segs[i].va, out, segs[i].size, false);
		out += segs[i].size;
		rh.out_segments++;
	}
	memcpy(rep, &rh, sizeof(rh));
	pages_release(c, &pages);
	*reply_bytes = rh.total_bytes;
	*result = ret;
	return 0;
}

int rt_lx_ioctl(struct rt_lx_client *c, int fd, uint32_t cmd,
		const void *frame, size_t frame_bytes, void *rep,
		size_t rep_cap, size_t *reply_bytes, int64_t *result)
{
	uint64_t out_bytes = 0;
	void *copy;
	int r;

	if (!c || !rep || !reply_bytes || !result)
		return -EINVAL;
	r = mlg_lx_frame_check(frame, frame_bytes, cmd, &out_bytes);
	if (r)
		return r;
	if (rep_cap < mlg_lx_reply_bytes(out_bytes))
		return -ENOSPC;
	/* The frame may live in memory the client can still write (a mapped
	 * input descriptor): work from a private copy, checked again. */
	copy = kvmalloc(frame_bytes, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, frame, frame_bytes);
	r = mlg_lx_frame_check(copy, frame_bytes, cmd, &out_bytes);
	if (!r && rep_cap < mlg_lx_reply_bytes(out_bytes))
		r = -ENOSPC;
	if (!r)
		r = call_begin(c);
	if (!r) {
		r = run_ioctl(c, fd, cmd, copy, frame_bytes, out_bytes, rep, reply_bytes, result);
		call_end(c);
	}
	kvfree(copy);
	return r;
}

/* ---- async ---- */

static void *async_main(void *arg)
{
	struct lx_async *a = arg;
	struct rt_lx_client *c = a->c;
	uint64_t out_bytes = 0;
	int consumed = 0;

	a->status = mlg_lx_frame_check(a->frame, a->frame_bytes, a->cmd, &out_bytes);
	if (!a->status) {
		a->rep = kvzalloc(mlg_lx_reply_bytes(out_bytes), GFP_KERNEL);
		a->status = a->rep ? 0 : -ENOMEM;
	}
	if (!a->status)
		a->status = run_ioctl(c, a->fd, a->cmd, a->frame, a->frame_bytes, out_bytes,
				      a->rep, &a->reply_bytes, &a->result);
	if (a->status) {
		/* The call did not run: report the transport error as its result. */
		struct mlg_lx_reply rh = {
			.magic = MLG_LX_REPLY_MAGIC, .version = MLG_LX_VERSION,
			.header_bytes = sizeof(rh), .total_bytes = sizeof(rh),
			.result = a->status,
		};

		kvfree(a->rep);
		a->rep = kvzalloc(sizeof(rh), GFP_KERNEL);
		if (a->rep)
			memcpy(a->rep, &rh, sizeof(rh));
		a->reply_bytes = a->rep ? sizeof(rh) : 0;
		a->result = a->status;
	}
	kvfree(a->frame);
	a->frame = NULL;
	if (a->done)
		consumed = a->done(a->ctx, a->token, a->result, a->rep, a->reply_bytes);
	pthread_mutex_lock(&c->lock);
	a->finished = true;
	if (consumed) {
		kvfree(a->rep);
		a->rep = NULL;
		a->reply_bytes = 0;
		a->done = NULL;	/* nothing left to fetch */
	}
	c->calls--;
	pthread_cond_broadcast(&c->changed);
	pthread_mutex_unlock(&c->lock);
	return NULL;
}

/* Join finished workers and drop delivered results. Caller holds c->lock;
 * the lock is dropped around joins. */
static void async_reap_locked(struct rt_lx_client *c)
{
	for (bool again = true; again;) {
		again = false;
		for (struct lx_async **link = &c->async; *link; link = &(*link)->next) {
			struct lx_async *a = *link;

			if (!a->finished)
				continue;
			if (!a->joined) {
				pthread_t thread = a->thread;

				a->joined = true;
				pthread_mutex_unlock(&c->lock);
				pthread_join(thread, NULL);
				pthread_mutex_lock(&c->lock);
				again = true;
				break;
			}
			if (!a->rep && !a->done) {
				*link = a->next;
				c->async_count--;
				kfree(a);
				again = true;
				break;
			}
		}
	}
}

int rt_lx_ioctl_async(struct rt_lx_client *c, int fd, uint32_t cmd,
		      const void *frame, size_t frame_bytes, rt_lx_done_fn done,
		      void *ctx, uint64_t *token)
{
	struct lx_async *a;
	uint64_t out_bytes;
	int r;

	if (!c || !token)
		return -EINVAL;
	r = mlg_lx_frame_check(frame, frame_bytes, cmd, &out_bytes);
	if (r)
		return r;
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->frame = kvmalloc(frame_bytes, GFP_KERNEL);
	if (!a->frame) {
		kfree(a);
		return -ENOMEM;
	}
	memcpy(a->frame, frame, frame_bytes);
	a->frame_bytes = frame_bytes;
	a->c = c;
	a->fd = fd;
	a->cmd = cmd;
	a->done = done;
	a->ctx = ctx;
	pthread_mutex_lock(&c->lock);
	async_reap_locked(c);
	if (c->dying) {
		r = -ESRCH;
	} else if (c->async_count >= MLG_LX_MAX_ASYNC) {
		r = -EAGAIN;
	} else {
		a->token = c->next_token++;
		c->calls++;
		r = pthread_create(&a->thread, NULL, async_main, a) ? -EAGAIN : 0;
		if (r) {
			c->calls--;
		} else {
			a->started = true;
			a->next = c->async;
			c->async = a;
			c->async_count++;
			*token = a->token;
		}
	}
	pthread_mutex_unlock(&c->lock);
	if (r) {
		kvfree(a->frame);
		kfree(a);
	}
	return r;
}

int rt_lx_result(struct rt_lx_client *c, uint64_t token, void *rep,
		 size_t cap, size_t *reply_bytes, int64_t *result)
{
	int r = -ENOENT;

	if (!c || !reply_bytes || !result)
		return -EINVAL;
	pthread_mutex_lock(&c->lock);
	for (struct lx_async *a = c->async; a; a = a->next) {
		if (a->token != token)
			continue;
		if (!a->finished) {
			r = -EBUSY;
		} else if (!a->rep) {
			r = -ENOENT;	/* already delivered */
		} else if (!rep || cap < a->reply_bytes) {
			*reply_bytes = a->reply_bytes;
			r = -ENOSPC;
		} else {
			memcpy(rep, a->rep, a->reply_bytes);
			*reply_bytes = a->reply_bytes;
			*result = a->result;
			kvfree(a->rep);
			a->rep = NULL;
			a->done = NULL;
			r = 0;
		}
		break;
	}
	async_reap_locked(c);
	pthread_mutex_unlock(&c->lock);
	return r;
}

unsigned int rt_lx_async_outstanding(struct rt_lx_client *c)
{
	unsigned int n;

	if (!c)
		return 0;
	pthread_mutex_lock(&c->lock);
	async_reap_locked(c);
	n = c->async_count;
	pthread_mutex_unlock(&c->lock);
	return n;
}

/* ---- mmap ---- */

int rt_lx_bar_of(struct pci_dev *pdev, uint64_t bus, uint64_t bytes, uint32_t *bar,
		 uint64_t *offset)
{
	for (uint32_t i = 0; i < PCI_ROM_RESOURCE; ++i) {
		uint64_t start = pci_resource_start(pdev, i);
		uint64_t len = pci_resource_len(pdev, i);

		if (!len || bus < start || bus - start >= len || bytes > len - (bus - start))
			continue;
		*bar = i;
		*offset = bus - start;
		return 0;
	}
	return -ERANGE;
}

static int map_add_range(struct lx_map *m, uint32_t bar, uint64_t addr, uint64_t bytes)
{
	struct lx_range *grown;

	if (m->nranges) {
		struct lx_range *last = &m->ranges[m->nranges - 1];

		if (last->bar == bar && last->addr + last->bytes == addr) {
			last->bytes += bytes;
			return 0;
		}
	}
	grown = krealloc(m->ranges, (m->nranges + 1) * sizeof(*grown), GFP_KERNEL);
	if (!grown)
		return -ENOMEM;
	m->ranges = grown;
	m->ranges[m->nranges++] = (struct lx_range){ .bar = bar, .addr = addr, .bytes = bytes };
	return 0;
}

static int map_sink(void *arg, uint32_t bar, uint64_t addr, uint64_t bytes)
{
	return map_add_range(arg, bar, addr, bytes);
}

/* A PFN mapping (KFD doorbells and MMIO, or kernel pages). */
static int map_pfn(struct rt_lx_client *c, struct lx_map *m)
{
	struct vm_area_struct *vma = m->vma;
	uint64_t bus = (uint64_t)vma->linuxu_pfn << PAGE_SHIFT;
	uint32_t bar;
	uint64_t offset;

	if (vma->linuxu_pfn_bytes != m->length)
		return -EFAULT;
	if (!rt_lx_bar_of(c->pdev, bus, m->length, &bar, &offset)) {
		m->backing = RT_LX_RANGE_BAR;
		m->cache = RT_LX_CACHE_UNCACHED;
		return map_add_range(m, bar, offset, m->length);
	}
	m->backing = RT_LX_RANGE_CPU;
	m->cache = RT_LX_CACHE_DEFAULT;
	for (uint64_t off = 0; off < m->length; off += PAGE_SIZE) {
		struct page *page = pfn_to_page(vma->linuxu_pfn + (off >> PAGE_SHIFT));
		void *cpu = page ? page_address(page) : NULL;
		int r = cpu ? map_add_range(m, 0, (uint64_t)(uintptr_t)cpu, PAGE_SIZE) : -EFAULT;

		if (r)
			return r;
	}
	return 0;
}

static void map_free(struct rt_lx_client *c, struct lx_map *m, bool in_mm)
{
	struct mm_struct *mm = linuxu_process_mm(c->proc);
	struct vm_area_struct *vma = m->vma;

	if (m->pinned)
		rt_lx_gem_unpin(m->pinned);
	if (vma) {
		if (in_mm) {
			mmap_write_lock(mm);
			linuxu_mm_remove_vma(mm, vma);
			mmap_write_unlock(mm);
		}
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		if (vma->vm_file)
			fput(vma->vm_file);
		vm_area_free(vma);
	}
	kfree(m->ranges);
	kfree(m);
}

int rt_lx_mmap(struct rt_lx_client *c, int fd, uint64_t offset, uint64_t length,
	       uint32_t prot, uint32_t flags, struct rt_lx_map_info *out)
{
	struct linuxu_process_saved saved;
	struct lx_map *m;
	struct file *file;
	uint32_t dev = 0;
	int r;

	if (!c || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!length || (offset | length) & ~PAGE_MASK || length > MLG_LX_VA_LIMIT ||
	    flags != MLG_LX_MAP_SHARED ||
	    (prot & ~(MLG_LX_PROT_READ | MLG_LX_PROT_WRITE)) || !prot)
		return -EINVAL;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->length = length;
	r = call_begin(c);
	if (r) {
		kfree(m);
		return r;
	}
	r = linuxu_process_enter(c->proc, &saved);
	if (r) {
		call_end(c);
		kfree(m);
		return r;
	}
	file = fget(fd);
	if (!file)
		r = -EBADF;
	else if (!(dev = file_device(c, file)) || !file->f_op || !file->f_op->mmap)
		r = -ENODEV;
	if (!r) {
		m->vma = vm_area_alloc(current->mm);
		if (!m->vma)
			r = -ENOMEM;
	}
	if (!r) {
		struct vm_area_struct *vma = m->vma;

		pthread_mutex_lock(&c->lock);
		vma->vm_start = c->pending_va;
		c->pending_va += length + PAGE_SIZE;
		if (c->pending_va >= TASK_SIZE_MAX - MLG_LX_VA_LIMIT / 2)
			c->pending_va = LX_PENDING_VA;
		pthread_mutex_unlock(&c->lock);
		vma->vm_end = vma->vm_start + length;
		vma->vm_pgoff = offset >> PAGE_SHIFT;
		vma->vm_flags = VM_SHARED | VM_MAYSHARE | VM_MAYREAD | VM_MAYWRITE |
			((prot & MLG_LX_PROT_READ) ? VM_READ : 0) |
			((prot & MLG_LX_PROT_WRITE) ? VM_WRITE : 0);
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		vma->vm_file = get_file(file);
		r = file->f_op->mmap(file, vma);
		if (r) {
			fput(vma->vm_file);
			vm_area_free(vma);
			m->vma = NULL;
		} else if (vma->vm_ops && vma->vm_ops->open) {
			/* mmap_region does not call ->open for the first VMA. */
		}
	}
	if (!r)
		r = m->vma->linuxu_pfn_bytes ? map_pfn(c, m) :
			dev == MLG_LX_DEV_RENDER ?
			rt_lx_gem_map(c->ddev, c->pdev, m->vma, m->length, &m->pinned,
				      &m->backing, &m->cache, map_sink, m) : -ENODEV;
	if (file)
		fput(file);
	linuxu_process_leave(&saved);
	if (r) {
		if (m->vma)
			map_free(c, m, false);
		else
			kfree(m);
		call_end(c);
		return (int)lx_errno(r);
	}
	pthread_mutex_lock(&c->lock);
	if (MLG_LX_MMAP_TYPE_BASE + c->next_map > MLG_LX_MMAP_TYPE_LIMIT) {
		r = -ENOMEM;
	} else {
		m->type = MLG_LX_MMAP_TYPE_BASE + c->next_map++;
		m->next = c->maps;
		c->maps = m;
		out->type = m->type;
		out->length = m->length;
		out->backing = m->backing;
		out->cache = m->cache;
		out->ranges = m->nranges;
	}
	pthread_mutex_unlock(&c->lock);
	if (r)
		map_free(c, m, false);
	call_end(c);
	return r;
}

static struct lx_map *map_find_locked(struct rt_lx_client *c, uint64_t type)
{
	for (struct lx_map *m = c->maps; m; m = m->next)
		if (m->type == type)
			return m;
	return NULL;
}

int rt_lx_map_info(struct rt_lx_client *c, uint64_t type, struct rt_lx_map_info *out)
{
	struct lx_map *m;

	if (!c || !out)
		return -EINVAL;
	pthread_mutex_lock(&c->lock);
	m = map_find_locked(c, type);
	if (m) {
		out->type = m->type;
		out->length = m->length;
		out->backing = m->backing;
		out->cache = m->cache;
		out->ranges = m->nranges;
		out->committed = m->committed;
		out->va = m->committed ? m->vma->vm_start : 0;
	}
	pthread_mutex_unlock(&c->lock);
	return m ? 0 : -ENOENT;
}

int rt_lx_map_ranges(struct rt_lx_client *c, uint64_t type,
		     int (*fn)(void *arg, uint32_t backing, uint32_t bar,
			       uint64_t addr, uint64_t bytes),
		     void *arg)
{
	struct lx_map *m;
	int r = 0;

	if (!c || !fn)
		return -EINVAL;
	pthread_mutex_lock(&c->lock);
	m = map_find_locked(c, type);
	for (uint32_t i = 0; m && !r && i < m->nranges; ++i)
		r = fn(arg, m->backing, m->ranges[i].bar, m->ranges[i].addr, m->ranges[i].bytes);
	pthread_mutex_unlock(&c->lock);
	return m ? r : -ENOENT;
}

void *rt_lx_map_cpu(struct rt_lx_client *c, uint64_t type, uint64_t offset,
		    uint64_t *contiguous)
{
	struct lx_map *m;
	void *cpu = NULL;

	if (!c)
		return NULL;
	pthread_mutex_lock(&c->lock);
	m = map_find_locked(c, type);
	if (m && m->backing == RT_LX_RANGE_CPU && offset < m->length) {
		for (uint32_t i = 0; i < m->nranges; ++i) {
			if (offset >= m->ranges[i].bytes) {
				offset -= m->ranges[i].bytes;
				continue;
			}
			cpu = (uint8_t *)(uintptr_t)m->ranges[i].addr + offset;
			if (contiguous)
				*contiguous = m->ranges[i].bytes - offset;
			break;
		}
	}
	pthread_mutex_unlock(&c->lock);
	return cpu;
}

int rt_lx_mmap_commit(struct rt_lx_client *c, uint64_t type, uint64_t va)
{
	struct mm_struct *mm;
	struct lx_map *m;
	int r;

	if (!c || (va & ~PAGE_MASK) || va < MLG_LX_VA_MIN || va >= MLG_LX_VA_LIMIT)
		return -EINVAL;
	mm = linuxu_process_mm(c->proc);
	pthread_mutex_lock(&c->lock);
	m = map_find_locked(c, type);
	if (!m) {
		r = -ENOENT;
	} else if (m->committed) {
		r = m->vma->vm_start == va ? 0 : -EBUSY;
	} else if (m->length > MLG_LX_VA_LIMIT - va) {
		r = -EINVAL;
	} else {
		uint64_t delta = va - m->vma->vm_start;

		m->vma->vm_start += delta;
		m->vma->vm_end += delta;
		mmap_write_lock(mm);
		r = linuxu_mm_insert_vma(mm, m->vma);
		mmap_write_unlock(mm);
		if (r) {
			m->vma->vm_start -= delta;
			m->vma->vm_end -= delta;
			r = r == -EEXIST ? -EBUSY : r;
		} else {
			m->committed = true;
		}
	}
	pthread_mutex_unlock(&c->lock);
	return r;
}

int rt_lx_munmap(struct rt_lx_client *c, uint64_t type)
{
	struct lx_map *m = NULL;

	if (!c)
		return -EINVAL;
	pthread_mutex_lock(&c->lock);
	for (struct lx_map **link = &c->maps; *link; link = &(*link)->next) {
		if ((*link)->type == type) {
			m = *link;
			*link = m->next;
			break;
		}
	}
	pthread_mutex_unlock(&c->lock);
	if (!m)
		return -ENOENT;
	map_free(c, m, m->committed);
	return 0;
}

unsigned int rt_lx_mappings(struct rt_lx_client *c)
{
	unsigned int n = 0;

	if (!c)
		return 0;
	pthread_mutex_lock(&c->lock);
	for (struct lx_map *m = c->maps; m; m = m->next)
		n++;
	pthread_mutex_unlock(&c->lock);
	return n;
}

/* ---- teardown ---- */

void rt_lx_client_destroy(struct rt_lx_client *c)
{
	if (!c)
		return;
	/* Whatever the client shows goes off the screen while its
	 * framebuffers still exist. */
	if (c->display)
		c->display->client_gone(c->pdev, c);
	pthread_mutex_lock(&c->lock);
	c->dying = true;
	pthread_mutex_unlock(&c->lock);
	/* Waits in the process return (-ERESTARTSYS, reported as EINTR). */
	linuxu_process_kill(c->proc);
	pthread_mutex_lock(&c->lock);
	while (c->calls)
		pthread_cond_wait(&c->changed, &c->lock);
	for (struct lx_async *a = c->async; a; a = a->next) {
		/* Results nobody fetches are dropped; their callbacks ran. */
		kvfree(a->rep);
		a->rep = NULL;
		a->done = NULL;
	}
	async_reap_locked(c);
	while (c->maps) {
		struct lx_map *m = c->maps;

		c->maps = m->next;
		pthread_mutex_unlock(&c->lock);
		map_free(c, m, m->committed);
		pthread_mutex_lock(&c->lock);
	}
	pthread_mutex_unlock(&c->lock);
	/* exit_mm, then exit_files: the files' release callbacks run. */
	linuxu_process_exit(c->proc);
	mmu_notifier_synchronize();
	pthread_mutex_destroy(&c->arena);
	pthread_cond_destroy(&c->changed);
	pthread_mutex_destroy(&c->lock);
	kfree(c);
}
