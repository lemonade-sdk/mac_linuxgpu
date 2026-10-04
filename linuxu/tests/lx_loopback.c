/* A libmlg_drm transport that calls the Linux-file core (rt/lx_files.h)
 * in-process, as the dext's selectors do: one client, waits on the core's
 * async workers, mmaps of CPU-backed memory handed back as the dext
 * mapping itself (host memory in the host build), and other mappings
 * (scattered pages, VRAM through a BAR) gathered into one range of the
 * same memory. LX_SCANOUT and the primary node go through the driver's
 * display hooks, as in the dext. For tests that run a libmlg_drm client
 * against the fixture device. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/pci.h>
#include <rt/display.h>
#include <rt/lx_files.h>

#include "mlg_drm.h"
#include "lx_loopback.h"

struct waiter {
	struct completion done;
	int64_t result;
	void *rbuf;
	size_t cap, bytes;
	int kept;
};

static struct rt_lx_client *client;
static unsigned int async_calls;
static void *(*bar_memory)(uint32_t bar, uint64_t offset, uint64_t bytes);

void lx_loopback_set_bar_memory(void *(*fn)(uint32_t bar, uint64_t offset, uint64_t bytes))
{
	bar_memory = fn;
}

/* Mach VM calls for gathering a scattered mapping into one range (the
 * kernel headers of this translation unit keep <mach/mach.h> out). */
extern unsigned int mach_task_self_;
extern int mach_vm_allocate(unsigned int task, uint64_t *addr, uint64_t size, int flags);
extern int mach_vm_deallocate(unsigned int task, uint64_t addr, uint64_t size);
extern int mach_vm_remap(unsigned int task, uint64_t *addr, uint64_t size, uint64_t mask,
			 int flags, unsigned int src_task, uint64_t src_addr, int copy,
			 int *cur, int *max, unsigned int inherit);
#define LB_VM_FLAGS_ANYWHERE	0x0001
#define LB_VM_FLAGS_FIXED	0x0000
#define LB_VM_FLAGS_OVERWRITE	0x4000
#define LB_VM_INHERIT_SHARE	0

/* Mappings gathered from several ranges, to be deallocated on munmap. */
struct gathered {
	struct gathered *next;
	uint64_t type, addr, length;
};
static struct gathered *gathered;
static pthread_mutex_t gathered_lock = PTHREAD_MUTEX_INITIALIZER;

struct gather {
	uint64_t base, at;
	int failed;
};

/* Place one range (CPU memory, or a BAR through bar_memory) at the next
 * address of the gathered mapping, sharing its pages. */
static int gather_range(void *arg, uint32_t backing, uint32_t bar, uint64_t addr, uint64_t bytes)
{
	struct gather *g = arg;
	uint64_t src = addr, dst = g->at;
	int cur, max;

	if (backing == RT_LX_RANGE_BAR) {
		void *p = bar_memory ? bar_memory(bar, addr, bytes) : NULL;

		if (!p) {
			g->failed = 1;
			return 1;
		}
		src = (uint64_t)(uintptr_t)p;
	}
	if (mach_vm_remap(mach_task_self_, &dst, bytes, 0,
			  LB_VM_FLAGS_FIXED | LB_VM_FLAGS_OVERWRITE, mach_task_self_, src, 0,
			  &cur, &max, LB_VM_INHERIT_SHARE)) {
		g->failed = 1;
		return 1;
	}
	g->at += bytes;
	return 0;
}

/* A mapping whose memory is not one contiguous CPU range (VRAM through a
 * BAR, scattered pages): its ranges gathered into fresh address space. */
static void *gather_mapping(uint64_t type, uint64_t length)
{
	struct gather g = { 0 };
	struct gathered *rec = calloc(1, sizeof(*rec));

	if (!rec || mach_vm_allocate(mach_task_self_, &g.base, length, LB_VM_FLAGS_ANYWHERE)) {
		free(rec);
		return NULL;
	}
	g.at = g.base;
	rt_lx_map_ranges(client, type, gather_range, &g);
	if (g.failed || g.at - g.base < length) {
		mach_vm_deallocate(mach_task_self_, g.base, length);
		free(rec);
		return NULL;
	}
	*rec = (struct gathered){ .type = type, .addr = g.base, .length = length };
	pthread_mutex_lock(&gathered_lock);
	rec->next = gathered;
	gathered = rec;
	pthread_mutex_unlock(&gathered_lock);
	return (void *)(uintptr_t)g.base;
}

static void release_gathered(uint64_t type)
{
	struct gathered *rec = NULL;

	pthread_mutex_lock(&gathered_lock);
	for (struct gathered **link = &gathered; *link; link = &(*link)->next) {
		if ((*link)->type == type) {
			rec = *link;
			*link = rec->next;
			break;
		}
	}
	pthread_mutex_unlock(&gathered_lock);
	if (rec) {
		mach_vm_deallocate(mach_task_self_, rec->addr, rec->length);
		free(rec);
	}
}

static int lb_open(void *ctx, uint32_t dev, uint32_t flags)
{
	(void)ctx;
	return rt_lx_open(client, dev, flags);
}

static int lb_close(void *ctx, int fd)
{
	(void)ctx;
	return rt_lx_close(client, fd);
}

static int lb_done(void *ctx, uint64_t token, int64_t result, const void *rbuf, size_t bytes)
{
	struct waiter *w = ctx;

	(void)token;
	w->result = result;
	w->bytes = bytes;
	if (bytes <= w->cap)
		memcpy(w->rbuf, rbuf, bytes);
	else
		w->kept = 1;
	complete(&w->done);
	return !w->kept;
}

static int lb_ioctl(void *ctx, int fd, uint32_t cmd, const void *frame, size_t bytes, void *rbuf,
		    size_t cap, size_t *reply_bytes, int64_t *result, int async)
{
	struct waiter w = { .rbuf = rbuf, .cap = cap };
	uint64_t token = 0;
	int r;

	(void)ctx;
	if (!async)
		return rt_lx_ioctl(client, fd, cmd, frame, bytes, rbuf, cap, reply_bytes, result);
	__atomic_add_fetch(&async_calls, 1, __ATOMIC_SEQ_CST);
	init_completion(&w.done);
	r = rt_lx_ioctl_async(client, fd, cmd, frame, bytes, lb_done, &w, &token);
	if (r)
		return r;
	wait_for_completion(&w.done);
	if (w.kept)
		return rt_lx_result(client, token, rbuf, cap, reply_bytes, result);
	*reply_bytes = w.bytes;
	*result = w.result;
	return 0;
}

static int lb_mmap(void *ctx, int fd, uint64_t offset, uint64_t length, uint32_t prot,
		   uint32_t flags, void **addr, uint64_t *handle)
{
	struct rt_lx_map_info info;
	uint64_t contiguous = 0;
	void *cpu;
	int r;

	(void)ctx;
	r = rt_lx_mmap(client, fd, offset, length, prot, flags, &info);
	if (r)
		return r;
	cpu = rt_lx_map_cpu(client, info.type, 0, &contiguous);
	if (!cpu || contiguous < length) {
		/* A BAR range or scattered pages: the dext maps those as a
		 * descriptor; the loopback gathers them with Mach VM. */
		cpu = gather_mapping(info.type, length);
		if (!cpu) {
			rt_lx_munmap(client, info.type);
			return -ENODEV;
		}
	}
	(void)rt_lx_mmap_commit(client, info.type, (uint64_t)(uintptr_t)cpu);
	*addr = cpu;
	*handle = info.type;
	return 0;
}

static int lb_munmap(void *ctx, uint64_t handle, void *addr, uint64_t length)
{
	(void)ctx;
	(void)addr;
	(void)length;
	release_gathered(handle);
	return rt_lx_munmap(client, handle);
}

static int lb_scanout(void *ctx, const struct mlg_lx_scanout *req, struct mlg_lx_scanout_state *state,
		      int64_t *result)
{
	(void)ctx;
	*result = rt_lx_scanout(client, req, state);
	return 0;
}

static struct pci_dev *lb_pdev;

/* The PCI function, as the IOKit transport reads it from the registry. */
static int lb_identity(void *ctx, struct mlg_pci_identity *out)
{
	(void)ctx;
	*out = (struct mlg_pci_identity){
		.domain = (uint16_t)pci_domain_nr(lb_pdev->bus),
		.bus = lb_pdev->bus->number,
		.dev = PCI_SLOT(lb_pdev->devfn),
		.func = PCI_FUNC(lb_pdev->devfn),
		.vendor_id = lb_pdev->vendor,
		.device_id = lb_pdev->device,
		.subvendor_id = lb_pdev->subsystem_vendor,
		.subdevice_id = lb_pdev->subsystem_device,
		.revision_id = lb_pdev->revision,
	};
	return 0;
}

int lx_loopback_transport(struct pci_dev *pdev, struct mlg_transport *out)
{
	int r = rt_lx_client_create(pdev, 0, "mlg-loopback", &client);

	if (r)
		return r;
	/* As the dext sets them for every Linux-file client. */
	rt_lx_client_set_display(client, &rt_display_lx_hooks);
	lb_pdev = pdev;
	*out = (struct mlg_transport){
		.open = lb_open, .close = lb_close, .ioctl = lb_ioctl,
		.mmap = lb_mmap, .munmap = lb_munmap, .identity = lb_identity,
		.scanout = lb_scanout,
	};
	return 0;
}

void lx_loopback_exit(void)
{
	rt_lx_client_destroy(client);
	client = NULL;
}

unsigned int lx_loopback_async_calls(void)
{
	return __atomic_load_n(&async_calls, __ATOMIC_SEQ_CST);
}

unsigned int lx_loopback_open_files(void)
{
	return rt_lx_open_files(client);
}
