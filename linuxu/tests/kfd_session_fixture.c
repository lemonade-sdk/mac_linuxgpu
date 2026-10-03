/* A fixture amdgpu device for upstream KFD tests (kfd_session_fixture.h).
 *
 * One GFX 12 node whose KGD memory calls (amdgpu_amdkfd_gpuvm_*) keep BOs
 * in a table and their VA mappings in a per-VM table that
 * amdgpu_vm_bo_lookup_mapping() searches; a MES whose add/remove_hw_queue
 * record what KFD's device queue manager sends; an SDMA copy that moves
 * bytes between fake MC ranges; a render node whose open creates the
 * amdgpu_fpriv ACQUIRE_VM takes (a PASID per open); a doorbell BAR in host
 * memory; and a command processor thread that executes AQL dispatch
 * packets of MES-added queues once their doorbell was written (completion
 * signal decrement, read index). Everything KFD reaches only with real
 * hardware is a tripwire. No DriverKit, MMIO or hardware. */
#include <assert.h>
/* libc usleep: <unistd.h> clashes with the kernel uuid_t. */
extern int usleep(unsigned int usec);
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/workqueue.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/ttm/ttm_tt.h>
#include <rt/chrdev.h>
#include <rt/compute.h>
#include <rt/kfd_session.h>
#include <rt/process.h>
#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "amdgpu_reset.h"
#include "mes_v12_api_def.h"
#include "amdgpu_vram_mgr.h"
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_topology.h"
#include "kfd_session_fixture.h"


#define TRIPWIRE(name) do { \
	fprintf(stderr, "kfd_session test: unexpected path %s reached\n", name); \
	abort(); \
} while (0)

/* ---- the device ---- */

struct amdgpu_device *adev;
struct kfd_dev kfd;
struct kfd_node node;
struct kfd_topology_device topo;
static struct amdgpu_reset_domain reset_domain;
static struct drm_minor render_minor = { .index = 128 };
uint64_t doorbell_bar[TEST_DOORBELL_BYTES / 8];
static uint8_t *vram;	/* host backing of the fake VRAM */
static uint64_t vram_next;

/* ---- the command processor: AQL dispatches of MES-added queues ---- */
struct cp_queue { bool live; uint32_t doorbell; uint64_t wptr; uint32_t pasid; };
static struct cp_queue cp_queues[16];
static pthread_mutex_t cp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t cp_thread;
static volatile bool cp_running;
unsigned int cp_dispatches;
static void cp_add(uint32_t doorbell, uint64_t wptr, uint32_t pasid)
{
	pthread_mutex_lock(&cp_lock);
	for (unsigned int i = 0; i < ARRAY_SIZE(cp_queues); ++i)
		if (!cp_queues[i].live) {
			cp_queues[i] = (struct cp_queue){ true, doorbell, wptr, pasid };
			break;
		}
	pthread_mutex_unlock(&cp_lock);
}
static void cp_remove(uint32_t doorbell)
{
	pthread_mutex_lock(&cp_lock);
	for (unsigned int i = 0; i < ARRAY_SIZE(cp_queues); ++i)
		if (cp_queues[i].live && cp_queues[i].doorbell == doorbell)
			cp_queues[i].live = false;
	pthread_mutex_unlock(&cp_lock);
}

/* ---- MES ----
 * A queue can be made hung (a wave that never preempts): REMOVE_QUEUE then
 * fails as MES's does when its preemption times out, until a hung-queue
 * reset (RESET with hang_detect_then_reset) reset it and the removal says
 * remove_queue_after_reset. A dead MES fails every call; a slow one takes
 * its time but answers within the API timeout. */
unsigned int mes_adds, mes_removes;
uint32_t mes_doorbells[16];
uint64_t mes_wptr[16], mes_page_table[16];
uint32_t mes_pasid[16];
static bool mes_hung[16], mes_was_reset[16], mes_live[16];
bool mes_dead;
unsigned int mes_remove_delay_us, mes_failed_removes, mes_hang_resets, mes_resumes,
	gpu_reset_requests;
static uint32_t mes_hung_db_array[8];
/* The queue MES knows by @doorbell: the latest added (KFD reuses the
 * doorbells of destroyed queues). */
static int mes_slot(uint32_t doorbell)
{
	for (unsigned int i = mes_adds; i-- > 0;)
		if (mes_doorbells[i] == doorbell)
			return (int)i;
	return -1;
}
void fixture_mes_hang(uint32_t doorbell, bool hung)
{
	int i = mes_slot(doorbell);

	assert(i >= 0);
	mes_hung[i] = hung;
}
/* Every queue MES schedules now, and every one added from now on while
 * @hung, has a wave that does not preempt. */
static bool mes_hang_new;
void fixture_mes_hang_all(bool hung)
{
	for (unsigned int i = 0; i < mes_adds; ++i)
		if (mes_live[i])
			mes_hung[i] = hung;
	mes_hang_new = hung;
}
unsigned int fixture_mes_failed_removes(void) { return mes_failed_removes; }
unsigned int fixture_mes_hang_resets(void) { return mes_hang_resets; }
static int fake_add_hw_queue(struct amdgpu_mes *mes, struct mes_add_queue_input *in)
{
	(void)mes;
	if (mes_dead)
		return -ETIMEDOUT;
	assert(mes_adds < ARRAY_SIZE(mes_doorbells));
	assert(in->is_kfd_process && in->is_aql_queue);
	assert(in->queue_type == MES_QUEUE_TYPE_COMPUTE);
	mes_doorbells[mes_adds] = in->doorbell_offset;
	mes_wptr[mes_adds] = in->wptr_addr;
	mes_page_table[mes_adds] = in->page_table_base_addr;
	mes_pasid[mes_adds] = in->process_id;
	mes_live[mes_adds] = true;
	mes_hung[mes_adds] = mes_hang_new;
	mes_was_reset[mes_adds] = false;
	mes_adds++;
	cp_add(in->doorbell_offset, in->wptr_addr, in->process_id);
	return 0;
}
static int fake_remove_hw_queue(struct amdgpu_mes *mes, struct mes_remove_queue_input *in)
{
	int i = mes_slot(in->doorbell_offset);

	(void)mes;
	assert(i >= 0);
	if (mes_remove_delay_us)
		usleep(mes_remove_delay_us);
	/* A queue MES does not schedule cannot be removed again. */
	if (mes_dead || !mes_live[i] ||
	    (mes_hung[i] && !(mes_was_reset[i] && in->remove_queue_after_reset))) {
		mes_failed_removes++;
		return -ETIMEDOUT;
	}
	mes_live[i] = false;
	mes_removes++;
	cp_remove(in->doorbell_offset);
	return 0;
}
/* MES_SCH_API_RESET, hang_detect_then_reset: every hung compute queue is
 * reset and reported in the hung-queue doorbell array. */
static int fake_detect_and_reset_hung_queues(struct amdgpu_mes *mes,
					     struct mes_detect_and_reset_queue_input *in)
{
	unsigned int n = 0;

	(void)mes;
	assert(in->queue_type == AMDGPU_RING_TYPE_COMPUTE && !in->detect_only);
	if (mes_dead)
		return -ETIMEDOUT;
	for (unsigned int i = 0; i < mes_adds; ++i) {
		if (mes_live[i] && mes_hung[i] && n < 4) {
			mes_was_reset[i] = true;
			mes_hung_db_array[n++] = mes_doorbells[i];
		}
	}
	mes_hang_resets++;
	return 0;
}
static int fake_map_legacy_queue(struct amdgpu_mes *mes, struct mes_map_legacy_queue_input *in)
{ (void)mes; (void)in; TRIPWIRE("MES MAP_LEGACY_QUEUE (a legacy HQD)"); }
static int fake_unmap_legacy_queue(struct amdgpu_mes *mes, struct mes_unmap_legacy_queue_input *in)
{ (void)mes; (void)in; TRIPWIRE("MES UNMAP_LEGACY_QUEUE"); }
static int fake_suspend_gang(struct amdgpu_mes *mes, struct mes_suspend_gang_input *in)
{ (void)mes; (void)in; TRIPWIRE("MES suspend_gang"); }
static int fake_resume_gang(struct amdgpu_mes *mes, struct mes_resume_gang_input *in)
{ (void)mes; (void)in; TRIPWIRE("MES resume_gang"); }
static int fake_misc_op(struct amdgpu_mes *mes, struct mes_misc_op_input *in)
{ (void)mes; (void)in; return 0; }
static const struct amdgpu_mes_funcs fake_mes_funcs = {
	.add_hw_queue = fake_add_hw_queue,
	.remove_hw_queue = fake_remove_hw_queue,
	.map_legacy_queue = fake_map_legacy_queue,
	.unmap_legacy_queue = fake_unmap_legacy_queue,
	.suspend_gang = fake_suspend_gang,
	.resume_gang = fake_resume_gang,
	.misc_op = fake_misc_op,
	.detect_and_reset_hung_queues = fake_detect_and_reset_hung_queues,
};
/* amdgpu_mes.c's wrappers the recovery calls (amdgpu_mes.c is not linked). */
int amdgpu_mes_detect_and_reset_hung_queues(struct amdgpu_device *a, int queue_type,
					    bool detect_only, unsigned int *hung_db_num,
					    u32 *hung_db_array, uint32_t xcc_id)
{
	struct mes_detect_and_reset_queue_input input = {0};
	u32 *db_array = a->mes.hung_queue_db_array_cpu_addr[xcc_id];
	int r;

	if (!hung_db_num || !hung_db_array)
		return -EINVAL;
	memset(db_array, 0xff, a->mes.hung_queue_db_array_size * sizeof(u32));
	input.queue_type = queue_type;
	input.detect_only = detect_only;
	r = a->mes.funcs->detect_and_reset_hung_queues(&a->mes, &input);
	if (!r) {
		*hung_db_num = 0;
		for (int i = 0; i < a->mes.hung_queue_hqd_info_offset; i++) {
			if (db_array[i] != AMDGPU_MES_INVALID_DB_OFFSET) {
				hung_db_array[i] = db_array[i];
				*hung_db_num += 1;
			}
		}
	}
	return r;
}
int amdgpu_mes_resume(struct amdgpu_device *a)
{
	(void)a;
	mes_resumes++;
	return mes_dead ? -ETIMEDOUT : 0;
}

/* ---- BOs and the VM ---- */
struct fake_bo {
	struct amdgpu_bo bo;
	struct amdgpu_bo_va bo_va;
	struct amdgpu_bo_va_mapping mapping;
	struct ttm_tt tt;
	struct amdgpu_vram_mgr_resource vres;
	struct gpu_buddy_block block;
	struct ttm_resource res;
	struct kgd_mem mem;
	struct page **pages;
	uint64_t gpu_offset;	/* kernel (VMID0) address, 0 if none */
	void *cpu;		/* kernel CPU mapping for kernel BOs */
	struct amdgpu_vm *vm;	/* the VM the BO is mapped in */
	bool mapped, kernel_mem;
	int refs;
};
#define MAX_BOS 128
static struct fake_bo *bos[MAX_BOS];
unsigned int live_bos, gart_maps;
static uint64_t kernel_gpu_next = TEST_KERNEL_GPU_BASE;

static struct fake_bo *fake_of(struct amdgpu_bo *bo)
{
	return container_of(bo, struct fake_bo, bo);
}

static void track(struct fake_bo *f)
{
	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (!bos[i]) { bos[i] = f; live_bos++; return; }
	abort();
}

static void untrack(struct fake_bo *f)
{
	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (bos[i] == f) { bos[i] = NULL; live_bos--; return; }
	abort();
}

static void bo_destroy(struct fake_bo *f)
{
	if (f->pages) {
		for (uint32_t i = 0; i < f->tt.num_pages; ++i)
			__free_pages(f->pages[i], 0);
		free(f->pages);
	}
	dma_resv_fini(&f->bo.tbo.base._resv);
	free(f->cpu);
	untrack(f);
	free(f);
}

/* A BO whose placement is @domain; GTT ones get system pages. */
static struct fake_bo *bo_create(uint64_t size, uint32_t domain)
{
	struct fake_bo *f = calloc(1, sizeof(*f));

	assert(f);
	size = ALIGN(size, PAGE_SIZE);
	f->bo.tbo.base.size = size;
	f->bo.tbo.bdev = &adev->mman.bdev;
	f->bo.tbo.base.resv = &f->bo.tbo.base._resv;
	dma_resv_init(&f->bo.tbo.base._resv);
	kref_init(&f->bo.tbo.base.refcount);
	f->refs = 1;
	f->bo.preferred_domains = domain;
	f->bo.allowed_domains = domain;
	if (domain == AMDGPU_GEM_DOMAIN_VRAM) {
		f->vres.base.mem_type = TTM_PL_VRAM;
		f->vres.base.size = size;
		INIT_LIST_HEAD(&f->vres.blocks);
		/* One buddy block whose order covers the BO. */
		unsigned int order = 0;
		while (((uint64_t)PAGE_SIZE << order) < size)
			order++;
		if (ALIGN(vram_next, (uint64_t)PAGE_SIZE << order) + ((uint64_t)PAGE_SIZE << order) >
		    TEST_VRAM_BYTES) {
			/* Out of VRAM, as TTM would report. */
			dma_resv_fini(&f->bo.tbo.base._resv);
			free(f);
			return NULL;
		}
		vram_next = ALIGN(vram_next, (uint64_t)PAGE_SIZE << order);
		f->block.header = vram_next | order;
		list_add(&f->block.link, &f->vres.blocks);
		vram_next += (uint64_t)PAGE_SIZE << order;
		f->bo.tbo.resource = &f->vres.base;
	} else {
		f->res.mem_type = domain == AMDGPU_GEM_DOMAIN_DOORBELL ?
			AMDGPU_PL_DOORBELL : TTM_PL_TT;
		f->res.size = size;
		f->bo.tbo.resource = &f->res;
		if (domain == AMDGPU_GEM_DOMAIN_GTT) {
			f->tt.num_pages = size / PAGE_SIZE;
			f->pages = calloc(f->tt.num_pages, sizeof(*f->pages));
			assert(f->pages);
			for (uint32_t i = 0; i < f->tt.num_pages; ++i) {
				f->pages[i] = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
				assert(f->pages[i]);
			}
			f->tt.pages = f->pages;
			f->tt.page_flags = TTM_TT_FLAG_PRIV_POPULATED;
			f->bo.tbo.ttm = &f->tt;
		}
	}
	track(f);
	return f;
}

/* amdgpu_object.c */
struct amdgpu_bo *amdgpu_bo_ref(struct amdgpu_bo *bo)
{
	if (bo)
		fake_of(bo)->refs++;
	return bo;
}
void amdgpu_bo_unref(struct amdgpu_bo **bo)
{
	if (!bo || !*bo)
		return;
	struct fake_bo *f = fake_of(*bo);
	*bo = NULL;
	if (--f->refs == 0)
		bo_destroy(f);
}
u64 amdgpu_bo_gpu_offset(struct amdgpu_bo *bo) { return fake_of(bo)->gpu_offset; }
u64 amdgpu_bo_gpu_offset_no_check(struct amdgpu_bo *bo) { return fake_of(bo)->gpu_offset; }
int amdgpu_bo_create_kernel(struct amdgpu_device *a, unsigned long size, int align,
			    u32 domain, struct amdgpu_bo **bo_ptr, u64 *gpu_addr, void **cpu_addr)
{
	(void)a; (void)align;
	struct fake_bo *f = bo_create(size, domain);
	if (domain == AMDGPU_GEM_DOMAIN_DOORBELL) {
		/* Process doorbell slices follow the kernel's on the BAR. */
		static uint64_t next_slice = 1;
		f->gpu_offset = next_slice++ * size;
	} else {
		f->gpu_offset = kernel_gpu_next;
		kernel_gpu_next += ALIGN(size, PAGE_SIZE);
		f->cpu = calloc(1, ALIGN(size, PAGE_SIZE));
	}
	*bo_ptr = &f->bo;
	if (gpu_addr)
		*gpu_addr = f->gpu_offset;
	if (cpu_addr)
		*cpu_addr = f->cpu;
	return 0;
}
void amdgpu_bo_free_kernel(struct amdgpu_bo **bo, u64 *gpu_addr, void **cpu_addr)
{
	if (!bo || !*bo)
		return;
	amdgpu_bo_unref(bo);
	if (gpu_addr)
		*gpu_addr = 0;
	if (cpu_addr)
		*cpu_addr = NULL;
}
uint32_t amdgpu_doorbell_index_on_bar(struct amdgpu_device *a, struct amdgpu_bo *db_bo,
				      uint32_t doorbell_index, uint32_t db_size)
{
	(void)a;
	return (uint32_t)(amdgpu_bo_gpu_offset_no_check(db_bo) / sizeof(u32)) +
	       doorbell_index * DIV_ROUND_UP(db_size, 4);
}

/* The VM of the render node's amdgpu_fpriv. */
static struct fake_bo *mapped[MAX_BOS];
struct amdgpu_bo_va_mapping *amdgpu_vm_bo_lookup_mapping(struct amdgpu_vm *vm, uint64_t addr)
{
	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (mapped[i] && mapped[i]->vm == vm &&
		    addr >= mapped[i]->mapping.start && addr <= mapped[i]->mapping.last)
			return &mapped[i]->mapping;
	return NULL;
}
struct amdgpu_bo_va *amdgpu_vm_bo_find(struct amdgpu_vm *vm, struct amdgpu_bo *bo)
{
	struct fake_bo *f = fake_of(bo);
	return f->mapped && f->vm == vm ? &f->bo_va : NULL;
}

/* ---- the render node: drm_open's amdgpu_driver_open_kms result ---- */
struct render_file {
	struct drm_file file;
	struct amdgpu_fpriv fpriv;
	struct fake_bo *root;
};
static struct render_file *render_files[8];
unsigned int render_opens, render_releases;
static int render_open(struct inode *inode, struct file *filp)
{
	struct render_file *r = calloc(1, sizeof(*r));

	assert(iminor(inode) == render_minor.index && MAJOR(inode->i_rdev) == DRM_MAJOR);
	assert(r);
	r->root = bo_create(PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM);
	r->fpriv.vm.root.bo = &r->root->bo;
	r->fpriv.vm.pasid = TEST_PASID + render_opens;
	r->file.driver_priv = &r->fpriv;
	filp->private_data = &r->file;
	for (unsigned int i = 0; i < ARRAY_SIZE(render_files); ++i)
		if (!render_files[i]) { render_files[i] = r; break; }
	render_opens++;
	return 0;
}
static int render_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	struct render_file *r = container_of((struct drm_file *)filp->private_data,
					     struct render_file, file);
	struct amdgpu_bo *root = &r->root->bo;
	for (unsigned int i = 0; i < ARRAY_SIZE(render_files); ++i)
		if (render_files[i] == r) render_files[i] = NULL;
	amdgpu_bo_unref(&root);
	free(r);
	render_releases++;
	return 0;
}
const struct file_operations render_fops = {
	.open = render_open,
	.release = render_release,
};
int amdgpu_file_to_fpriv(struct file *filp, struct amdgpu_fpriv **fpriv)
{
	if (!filp || filp->f_op != &render_fops)
		return -EINVAL;
	*fpriv = ((struct drm_file *)filp->private_data)->driver_priv;
	return 0;
}

/* ---- KGD memory (amdgpu_amdkfd_gpuvm.c) ---- */
struct process_info { int unused; };
unsigned int vm_acquires;
static const char *fence_name(struct dma_fence *f) { (void)f; return "kfd-eviction"; }
static const struct dma_fence_ops eviction_fence_ops = {
	.get_driver_name = fence_name,
	.get_timeline_name = fence_name,
};
static spinlock_t eviction_lock;
int amdgpu_amdkfd_gpuvm_acquire_process_vm(struct amdgpu_device *a, struct amdgpu_vm *avm,
					   void **process_info, struct dma_fence **ef)
{
	assert(a == adev && avm->pasid >= TEST_PASID);
	if (!*process_info) {
		*process_info = calloc(1, sizeof(struct process_info));
		assert(*process_info);
	}
	if (ef) {
		struct dma_fence *fence = kzalloc(sizeof(*fence), GFP_KERNEL);
		assert(fence);
		spin_lock_init(&eviction_lock);
		dma_fence_init(fence, &eviction_fence_ops, &eviction_lock, 1, 1);
		*ef = fence;
	}
	vm_acquires++;
	return 0;
}
void amdgpu_amdkfd_gpuvm_release_process_vm(struct amdgpu_device *a, void *drm_priv)
{ (void)a; (void)drm_priv; }
void amdgpu_amdkfd_gpuvm_destroy_cb(struct amdgpu_device *a, struct amdgpu_vm *vm)
{ (void)a; (void)vm; }

uint64_t amdgpu_amdkfd_gpuvm_get_process_page_dir(void *drm_priv)
{
	(void)drm_priv;
	return 0x7e57000ULL;	/* the page directory MES receives */
}
static uint64_t vram_used;
size_t amdgpu_amdkfd_get_available_memory(struct amdgpu_device *a, uint8_t xcp_id)
{ (void)a; (void)xcp_id; return TEST_VRAM_BYTES - vram_used; }
unsigned int kgd_allocs, kgd_frees, kgd_maps, kgd_unmaps;
int amdgpu_amdkfd_gpuvm_alloc_memory_of_gpu(struct amdgpu_device *a, uint64_t va,
		uint64_t size, void *drm_priv, struct kgd_mem **mem, uint64_t *offset,
		uint32_t flags, bool criu_resume)
{
	uint32_t domain = (flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM) ? AMDGPU_GEM_DOMAIN_VRAM :
			  AMDGPU_GEM_DOMAIN_GTT;
	struct fake_bo *f;

	(void)a; (void)drm_priv; (void)criu_resume;
	assert(!(flags & (KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL | KFD_IOC_ALLOC_MEM_FLAGS_MMIO_REMAP |
			  KFD_IOC_ALLOC_MEM_FLAGS_USERPTR)));
	assert(size && !(size & (PAGE_SIZE - 1)) && !(va & (PAGE_SIZE - 1)));
	f = bo_create(size, domain);
	if (!f)
		return -ENOMEM;
	mutex_init(&f->mem.lock);
	f->mem.bo = &f->bo;
	f->mem.va = va;
	f->mem.domain = domain;
	f->mem.alloc_flags = flags;
	if (domain == AMDGPU_GEM_DOMAIN_VRAM)
		vram_used += size;
	*mem = &f->mem;
	if (offset)
		*offset = 0;
	kgd_allocs++;
	return 0;
}
int amdgpu_amdkfd_gpuvm_free_memory_of_gpu(struct amdgpu_device *a, struct kgd_mem *mem,
		void *drm_priv, uint64_t *size)
{
	struct fake_bo *f = container_of(mem, struct fake_bo, mem);
	struct amdgpu_bo *bo = &f->bo;

	(void)a; (void)drm_priv;
	assert(!f->mapped);
	if (size)
		*size = mem->domain == AMDGPU_GEM_DOMAIN_VRAM ? f->bo.tbo.base.size : 0;
	if (mem->domain == AMDGPU_GEM_DOMAIN_VRAM)
		vram_used -= f->bo.tbo.base.size;
	mutex_destroy(&mem->lock);
	kgd_frees++;
	amdgpu_bo_unref(&bo);
	return 0;
}
int amdgpu_amdkfd_gpuvm_map_memory_to_gpu(struct amdgpu_device *a, struct kgd_mem *mem,
					  void *drm_priv)
{
	struct fake_bo *f = container_of(mem, struct fake_bo, mem);

	(void)a;
	assert(!f->mapped);
	f->vm = drm_priv_to_vm(drm_priv);
	f->bo_va.base.bo = &f->bo;
	f->mapping.bo_va = &f->bo_va;
	f->mapping.start = mem->va >> AMDGPU_GPU_PAGE_SHIFT;
	f->mapping.last = ((mem->va + f->bo.tbo.base.size) >> AMDGPU_GPU_PAGE_SHIFT) - 1;
	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (!mapped[i]) { mapped[i] = f; break; }
	f->mapped = true;
	mem->mapped_to_gpu_memory = 1;
	kgd_maps++;
	return 0;
}
int amdgpu_amdkfd_gpuvm_unmap_memory_from_gpu(struct amdgpu_device *a, struct kgd_mem *mem,
					      void *drm_priv)
{
	struct fake_bo *f = container_of(mem, struct fake_bo, mem);

	(void)a; (void)drm_priv;
	if (!f->mapped)
		return -EINVAL;
	/* KFD only lets user space unmap BOs no queue holds. */
	assert(!f->bo_va.queue_refcount);
	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (mapped[i] == f) mapped[i] = NULL;
	f->mapped = false;
	mem->mapped_to_gpu_memory = 0;
	kgd_unmaps++;
	return 0;
}
int amdgpu_amdkfd_gpuvm_dmaunmap_mem(struct kgd_mem *mem, void *drm_priv)
{ (void)mem; (void)drm_priv; return 0; }
int amdgpu_amdkfd_gpuvm_sync_memory(struct amdgpu_device *a, struct kgd_mem *mem, bool intr)
{ (void)a; (void)mem; (void)intr; return 0; }
bool amdgpu_amdkfd_bo_mapped_to_dev(void *drm_priv, struct kgd_mem *mem)
{ (void)drm_priv; return container_of(mem, struct fake_bo, mem)->mapped; }
int amdgpu_amdkfd_gpuvm_map_gtt_bo_to_kernel(struct kgd_mem *mem, void **kptr, uint64_t *size)
{
	struct fake_bo *f = container_of(mem, struct fake_bo, mem);

	/* KFD maps its own one-page BOs (CWSR TBA/TMA). */
	assert(f->pages && f->tt.num_pages == 1);
	*kptr = page_address(f->pages[0]);
	if (size)
		*size = f->bo.tbo.base.size;
	return 0;
}
void amdgpu_amdkfd_gpuvm_unmap_gtt_bo_from_kernel(struct kgd_mem *mem) { (void)mem; }
int amdgpu_amdkfd_map_gtt_bo_to_gart(struct amdgpu_bo *bo, struct amdgpu_bo **bo_gart)
{
	/* MES polls the write pointer through the GART. */
	fake_of(bo)->gpu_offset = kernel_gpu_next;
	kernel_gpu_next += PAGE_SIZE;
	*bo_gart = amdgpu_bo_ref(bo);
	gart_maps++;
	return 0;
}
int amdgpu_amdkfd_gpuvm_restore_process_bos(void *process_info, struct dma_fence __rcu **ef)
{ (void)process_info; (void)ef; TRIPWIRE("restore_process_bos (no eviction in this test)"); }
void amdgpu_amdkfd_gpuvm_destroy_process_info_fixture(void);

/* KFD's kernel-owned GTT (amdgpu_amdkfd_alloc_kernel_mem): MES process and
 * gang contexts, MQDs. */
struct kernel_mem { struct amdgpu_bo *bo; };
unsigned int kernel_allocs;
int amdgpu_amdkfd_alloc_kernel_mem(struct amdgpu_device *a, size_t size, u32 domain,
				   void **mem_obj, uint64_t *gpu_addr, void **cpu_ptr, bool mqd_gfx9)
{
	struct amdgpu_bo *bo;
	void *cpu;
	uint64_t gpu;

	(void)mqd_gfx9;
	assert(domain == AMDGPU_GEM_DOMAIN_GTT);
	assert(!amdgpu_bo_create_kernel(a, size, PAGE_SIZE, AMDGPU_GEM_DOMAIN_CPU, &bo, &gpu, &cpu));
	fake_of(bo)->kernel_mem = true;
	*mem_obj = bo;
	*gpu_addr = gpu;
	*cpu_ptr = cpu;
	kernel_allocs++;
	return 0;
}
void amdgpu_amdkfd_free_kernel_mem(struct amdgpu_device *a, void **mem_obj)
{
	struct amdgpu_bo *bo = *mem_obj;

	(void)a;
	if (!bo)
		return;
	/* Also how KFD drops a write pointer's GART mapping. */
	if (fake_of(bo)->kernel_mem)
		kernel_allocs--;
	amdgpu_bo_free_kernel(&bo, NULL, NULL);
	*mem_obj = NULL;
}
/* kfd_device.c's GTT sub-allocator: a chunk of KFD's kernel GTT, its
 * kfd_mem_obj without a BO of its own (mem == NULL). */
struct sa_chunk { struct kfd_mem_obj obj; struct amdgpu_bo *bo; };
int kfd_gtt_sa_allocate(struct kfd_node *n, unsigned int size, struct kfd_mem_obj **mem_obj)
{
	struct sa_chunk *chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
	void *cpu;
	uint64_t gpu;

	(void)n;
	assert(chunk);
	assert(!amdgpu_bo_create_kernel(adev, size, PAGE_SIZE, AMDGPU_GEM_DOMAIN_CPU,
					&chunk->bo, &gpu, &cpu));
	chunk->obj.gpu_addr = gpu;
	chunk->obj.cpu_ptr = cpu;
	*mem_obj = &chunk->obj;
	kernel_allocs++;
	return 0;
}
int kfd_gtt_sa_free(struct kfd_node *n, struct kfd_mem_obj *mem_obj)
{
	struct sa_chunk *chunk;

	(void)n;
	if (!mem_obj)
		return 0;
	chunk = container_of(mem_obj, struct sa_chunk, obj);
	amdgpu_bo_free_kernel(&chunk->bo, NULL, NULL);
	kfree(chunk);
	kernel_allocs--;
	return 0;
}

/* ---- topology: one GPU node ---- */
uint32_t kfd_gpu_node_num(void) { return 1; }
bool kfd_is_locked(struct kfd_dev *k) { (void)k; return false; }
uint32_t kfd_topology_get_num_devices(void) { return 1; }
int kfd_topology_enum_kfd_devices(uint8_t idx, struct kfd_node **kdev)
{
	if (idx > 0)
		return -1;
	*kdev = &node;
	return 0;
}
struct kfd_node *kfd_device_by_id(uint32_t gpu_id)
{
	return gpu_id == TEST_GPU_ID ? &node : NULL;
}
struct kfd_topology_device *kfd_topology_device_by_id(uint32_t gpu_id)
{
	return gpu_id == TEST_GPU_ID ? &topo : NULL;
}

/* ---- the staging context (rt/compute.h) and SDMA ---- */
struct rt_compute_bo { void *cpu; uint64_t gpu; uint64_t size; };
struct rt_compute_ctx compute_ctx;
#define MAX_STAGING 4
static struct rt_compute_bo *stagings[MAX_STAGING];
static uint64_t staging_next = 0x100000000ULL;	/* GART addresses */
int rt_compute_bo_alloc(struct rt_compute_ctx *ctx, uint64_t size, uint64_t alignment,
			enum rt_compute_domain domain, struct rt_compute_bo **out)
{
	struct rt_compute_bo *bo = calloc(1, sizeof(*bo));

	(void)alignment;
	assert(ctx == &compute_ctx && domain == RT_COMPUTE_GTT && bo);
	bo->cpu = calloc(1, size);
	bo->gpu = staging_next;
	staging_next += ALIGN(size, PAGE_SIZE);
	bo->size = size;
	for (unsigned int i = 0; i < MAX_STAGING; ++i)
		if (!stagings[i]) { stagings[i] = bo; break; }
	ctx->bos++;
	*out = bo;
	return 0;
}
int rt_compute_bo_info(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo,
		       struct rt_compute_bo_info *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	out->cpu_address = bo->cpu;
	out->gpu_address = bo->gpu;
	out->size = bo->size;
	out->domain = RT_COMPUTE_GTT;
	return 0;
}
int rt_compute_bo_free(struct rt_compute_ctx *ctx, struct rt_compute_bo *bo)
{
	bool found = false;
	for (unsigned int i = 0; i < MAX_STAGING; ++i)
		if (stagings[i] == bo) { stagings[i] = NULL; found = true; }
	assert(found);
	free(bo->cpu);
	free(bo);
	ctx->bos--;
	return 0;
}
uint64_t amdgpu_ttm_domain_start(struct amdgpu_device *a, uint32_t type)
{
	assert(type == TTM_PL_VRAM);
	return a->gmc.vram_start;
}
static void *mc_to_host(uint64_t mc, uint64_t bytes)
{
	for (unsigned int i = 0; i < MAX_STAGING; ++i) {
		struct rt_compute_bo *bo = stagings[i];
		if (bo && mc >= bo->gpu && mc + bytes <= bo->gpu + bo->size)
			return (char *)bo->cpu + (mc - bo->gpu);
	}
	assert(mc >= TEST_VRAM_START && mc + bytes <= TEST_VRAM_START + TEST_VRAM_BYTES);
	return vram + (mc - TEST_VRAM_START);
}
unsigned int sdma_copies;
bool sdma_hold;
static struct {
	struct dma_fence *fence;
	uint64_t src, dst;
	uint32_t bytes;
} sdma_pending[8];
static unsigned int sdma_npending;
static spinlock_t sdma_lock;
static const char *sdma_name(struct dma_fence *f) { (void)f; return "sdma"; }
static const struct dma_fence_ops sdma_fence_ops = {
	.get_driver_name = sdma_name,
	.get_timeline_name = sdma_name,
};
int amdgpu_copy_buffer(struct amdgpu_device *a, struct amdgpu_ttm_buffer_entity *entity,
		       uint64_t src, uint64_t dst, uint32_t bytes, struct dma_resv *resv,
		       struct dma_fence **fence, bool vm_needs_flush, uint32_t copy_flags)
{
	/* dma_fence_free releases it with kfree_rcu. */
	struct dma_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

	(void)a; (void)vm_needs_flush; (void)copy_flags;
	assert(entity == &adev->mman.default_entity && !resv);
	dma_fence_init(f, &sdma_fence_ops, &sdma_lock, 2, ++sdma_copies);
	if (sdma_hold) {
		/* The engine has not got to it yet. */
		assert(sdma_npending < ARRAY_SIZE(sdma_pending));
		sdma_pending[sdma_npending].fence = dma_fence_get(f);
		sdma_pending[sdma_npending].src = src;
		sdma_pending[sdma_npending].dst = dst;
		sdma_pending[sdma_npending++].bytes = bytes;
	} else {
		memmove(mc_to_host(dst, bytes), mc_to_host(src, bytes), bytes);
		dma_fence_signal(f);
	}
	*fence = f;
	return 0;
}
/* The held engine catches up: every pending copy runs and signals. */
void fixture_sdma_release(void)
{
	for (unsigned int i = 0; i < sdma_npending; ++i) {
		memmove(mc_to_host(sdma_pending[i].dst, sdma_pending[i].bytes),
			mc_to_host(sdma_pending[i].src, sdma_pending[i].bytes),
			sdma_pending[i].bytes);
		dma_fence_signal(sdma_pending[i].fence);
		dma_fence_put(sdma_pending[i].fence);
	}
	sdma_npending = 0;
	sdma_hold = false;
}


/* ---- amdgpu module parameters KFD reads (amdgpu_drv.c defaults) ---- */
int sched_policy = KFD_SCHED_POLICY_HWS;
int max_num_of_queues_per_device = KFD_MAX_NUM_OF_QUEUES_PER_DEVICE_DEFAULT;
int halt_if_hws_hang;
bool debug_evictions;
int queue_preemption_timeout_ms = 9000;
uint amdgpu_sdma_phase_quantum = 32;
int amdgpu_gpu_recovery = -1;
/* drm/ttm.c's DMA ops: only referenced by the device model here. */
const char linuxu_dma_ops = 0;

void __drm_err(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/* ---- amdgpu services KFD calls on these paths ---- */
int amdgpu_mes_doorbell_process_slice(struct amdgpu_device *a)
{
	(void)a;
	/* amdgpu_mes.c: 8-byte doorbells for 1024 queues per process. */
	return roundup(8 * 1024, PAGE_SIZE);
}
void amdgpu_queue_mask_bit_to_mec_queue(struct amdgpu_device *a, int bit,
					int *mec, int *pipe, int *queue)
{
	*queue = bit % a->gfx.mec.num_queue_per_pipe;
	*pipe = (bit / a->gfx.mec.num_queue_per_pipe) % a->gfx.mec.num_pipe_per_mec;
	*mec = (bit / a->gfx.mec.num_queue_per_pipe) / a->gfx.mec.num_pipe_per_mec;
}
int amdgpu_queue_mask_bit_to_set_resource_bit(struct amdgpu_device *a, int queue_bit)
{
	int mec, pipe, queue;

	amdgpu_queue_mask_bit_to_mec_queue(a, queue_bit, &mec, &pipe, &queue);
	return mec * 4 * 8 + pipe * 8 + queue;
}
int amdgpu_in_reset(struct amdgpu_device *a) { (void)a; return 0; }
bool amdgpu_amdkfd_have_atomics_support(struct amdgpu_device *a) { (void)a; return false; }
bool amdgpu_amdkfd_is_fed(struct amdgpu_device *a) { (void)a; return false; }
uint64_t amdgpu_amdkfd_get_gpu_clock_counter(struct amdgpu_device *a) { (void)a; return 1; }
static unsigned int tlb_flushes;
int amdgpu_vm_flush_compute_tlb(struct amdgpu_device *a, struct amdgpu_vm *vm,
				uint32_t flush_type, uint32_t xcc_mask)
{ (void)a; (void)vm; (void)flush_type; (void)xcc_mask; tlb_flushes++; return 0; }
void ttm_bo_move_to_lru_tail(struct ttm_buffer_object *bo) { (void)bo; }
void amdgpu_amdkfd_debug_mem_fence(struct amdgpu_device *a) { (void)a; }
void amdgpu_device_flush_hdp(struct amdgpu_device *a, struct amdgpu_ring *ring)
{ (void)a; (void)ring; }
int amdgpu_amdkfd_get_tile_config(struct amdgpu_device *a, struct tile_config *config)
{ (void)a; (void)config; TRIPWIRE("get_tile_config"); }
int amdgpu_amdkfd_get_dmabuf_info(struct amdgpu_device *a, int dma_buf_fd,
		struct amdgpu_device **dmabuf_adev, uint64_t *bo_size, void *metadata_buffer,
		size_t buffer_size, uint32_t *metadata_size, uint32_t *flags, int8_t *xcp_id)
{
	(void)a; (void)dma_buf_fd; (void)dmabuf_adev; (void)bo_size; (void)metadata_buffer;
	(void)buffer_size; (void)metadata_size; (void)flags; (void)xcp_id;
	TRIPWIRE("get_dmabuf_info");
}
int amdgpu_amdkfd_gpuvm_import_dmabuf_fd(struct amdgpu_device *a, int fd, uint64_t va,
		void *drm_priv, struct kgd_mem **mem, uint64_t *size, uint64_t *mmap_offset)
{
	(void)a; (void)fd; (void)va; (void)drm_priv; (void)mem; (void)size; (void)mmap_offset;
	TRIPWIRE("import_dmabuf");
}
int amdgpu_amdkfd_gpuvm_export_dmabuf(struct kgd_mem *mem, struct dma_buf **dmabuf)
{ (void)mem; (void)dmabuf; TRIPWIRE("export_dmabuf"); }
int dma_buf_fd(struct dma_buf *dmabuf, int flags) { (void)dmabuf; (void)flags; TRIPWIRE("dma_buf_fd"); }
void dma_buf_put(struct dma_buf *dmabuf) { (void)dmabuf; TRIPWIRE("dma_buf_put"); }
int amdgpu_amdkfd_add_gws_to_process(void *info, void *gws, struct kgd_mem **mem)
{ (void)info; (void)gws; (void)mem; TRIPWIRE("add_gws_to_process"); }
int amdgpu_amdkfd_remove_gws_from_process(void *info, void *mem)
{ (void)info; (void)mem; TRIPWIRE("remove_gws_from_process"); }
void amdgpu_amdkfd_block_mmu_notifications(void *p) { (void)p; TRIPWIRE("criu block_mmu_notifications"); }
int amdgpu_amdkfd_criu_resume(void *p) { (void)p; TRIPWIRE("criu_resume"); }
/* kfd_hws_hang's GPU reset request. With amdgpu_gpu_recovery = 0, as
 * linuxu_driver_bootstrap sets it, amdgpu_device_should_recover_gpu only
 * logs, so the request changes nothing. */
void amdgpu_amdkfd_gpu_reset(struct amdgpu_device *a)
{
	(void)a;
	gpu_reset_requests++;
	fprintf(stderr, "kfd fixture: GPU reset requested; GPU recovery disabled.\n");
}
int amdgpu_amdkfd_send_close_event_drain_irq(struct amdgpu_device *a, uint32_t *payload)
{ (void)a; (void)payload; TRIPWIRE("send_close_event_drain_irq"); }
int amdgpu_amdkfd_submit_ib(struct amdgpu_device *a, enum kgd_engine_type engine,
			    uint32_t vmid, uint64_t gpu_addr, uint32_t *ib_cmd, uint32_t ib_len)
{ (void)a; (void)engine; (void)vmid; (void)gpu_addr; (void)ib_cmd; (void)ib_len; TRIPWIRE("submit_ib"); }
void amdgpu_gfx_off_ctrl(struct amdgpu_device *a, bool enable)
{ (void)a; (void)enable; TRIPWIRE("gfx_off_ctrl (ttmp setup)"); }
/* The process teardown's MES context flush (pqm's dequeue). */
unsigned int mes_shader_debugger_flushes;
int amdgpu_mes_flush_shader_debugger(struct amdgpu_device *a, uint64_t p, uint32_t x)
{ (void)a; (void)x; assert(p); mes_shader_debugger_flushes++; return 0; }
/* RUNTIME_ENABLE's MES SET_SHADER_DEBUGGER (clears the process context). */
unsigned int mes_shader_debugger_sets;
int amdgpu_mes_set_shader_debugger(struct amdgpu_device *a, uint64_t p, uint32_t s,
		const uint32_t *t, uint32_t f, bool trap_en, uint32_t x)
{
	(void)a; (void)s; (void)t; (void)f; (void)trap_en; (void)x;
	assert(p);
	mes_shader_debugger_sets++;
	return 0;
}
int amdgpu_sdma_reset_engine(struct amdgpu_device *a, uint32_t instance_id, bool caller_handles_kernel_queues)
{ (void)a; (void)instance_id; (void)caller_handles_kernel_queues; TRIPWIRE("sdma_reset_engine"); }
bool amdgpu_sriov_xnack_support(struct amdgpu_device *a) { (void)a; TRIPWIRE("sriov_xnack_support"); }
int amdgpu_ttm_tt_get_userptr(const struct ttm_buffer_object *tbo, uint64_t *user_addr)
{ (void)tbo; (void)user_addr; TRIPWIRE("ttm_tt_get_userptr"); }
/* SMI process events: no task info recorded on this VM. */
struct amdgpu_task_info *amdgpu_vm_get_task_info_vm(struct amdgpu_vm *vm)
{ (void)vm; return NULL; }
void amdgpu_vm_put_task_info(struct amdgpu_task_info *t) { (void)t; TRIPWIRE("vm_put_task_info"); }
unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len,
		      unsigned long prot, unsigned long flags, unsigned long offset)
{ (void)file; (void)addr; (void)len; (void)prot; (void)flags; (void)offset; TRIPWIRE("vm_mmap (APU CWSR)"); }

/* ---- kfd_device.c ---- */
unsigned int kfd_get_num_sdma_engines(struct kfd_node *n)
{ return n->adev->sdma.num_instances / (int)n->kfd->num_nodes; }
unsigned int kfd_get_num_xgmi_sdma_engines(struct kfd_node *n) { (void)n; return 0; }
static int compute_active;
void kfd_inc_compute_active(struct kfd_node *n) { (void)n; compute_active++; }
void kfd_dec_compute_active(struct kfd_node *n) { (void)n; compute_active--; }

/* ---- other families and the HWS packet manager: MES devices never use them ---- */
void device_queue_manager_init_cik(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm cik"); }
void device_queue_manager_init_vi(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm vi"); }
void device_queue_manager_init_v9(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm v9"); }
void device_queue_manager_init_v10(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm v10"); }
void device_queue_manager_init_v11(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm v11"); }
void device_queue_manager_init_v12_1(struct device_queue_manager_asic_ops *o) { (void)o; TRIPWIRE("dqm v12.1"); }
int pm_init(struct packet_manager *pm, struct device_queue_manager *dqm)
{ (void)pm; (void)dqm; TRIPWIRE("pm_init (HWS without MES)"); }
void pm_uninit(struct packet_manager *pm) { (void)pm; TRIPWIRE("pm_uninit"); }
int pm_send_set_resources(struct packet_manager *pm, struct scheduling_resources *res)
{ (void)pm; (void)res; TRIPWIRE("pm_send_set_resources"); }
int pm_send_runlist(struct packet_manager *pm, struct list_head *dqm_queues)
{ (void)pm; (void)dqm_queues; TRIPWIRE("pm_send_runlist"); }
int pm_send_query_status(struct packet_manager *pm, uint64_t fence_address, uint64_t fence_value)
{ (void)pm; (void)fence_address; (void)fence_value; TRIPWIRE("pm_send_query_status"); }
int pm_send_unmap_queue(struct packet_manager *pm, enum kfd_unmap_queues_filter mode,
			uint32_t filter_param, bool reset)
{ (void)pm; (void)mode; (void)filter_param; (void)reset; TRIPWIRE("pm_send_unmap_queue"); }
void pm_release_ib(struct packet_manager *pm) { (void)pm; TRIPWIRE("pm_release_ib"); }
int pm_config_dequeue_wait_counts(struct packet_manager *pm, enum kfd_config_dequeue_wait_counts_cmd cmd,
				  uint32_t value)
{ (void)pm; (void)cmd; (void)value; TRIPWIRE("pm_config_dequeue_wait_counts"); }
void kernel_queue_uninit(struct kernel_queue *kq) { (void)kq; TRIPWIRE("kernel_queue_uninit"); }
int kfd_debugfs_hqds_by_device(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_hqds"); }
int kfd_debugfs_rls_by_device(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_rls"); }
int kfd_debugfs_hang_hws(struct kfd_node *dev) { (void)dev; TRIPWIRE("debugfs_hang_hws"); }
int kfd_debugfs_kfd_mem_limits(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_mem_limits"); }

/* ---- device bring-up: what kgd2kfd_device_init leaves behind ---- */
static struct amdgpu_ring sdma_ring;
/* The KGD register calls KFD makes for a GFX 12 node (gfx_v12_kfd2kgd's
 * entries reached here). */
static unsigned int kgd_interrupt_inits;
static int kgd_init_interrupts(struct amdgpu_device *a, uint32_t pipe_id, uint32_t inst)
{ (void)a; (void)pipe_id; (void)inst; kgd_interrupt_inits++; return 0; }
static uint32_t kgd_enable_debug_trap(struct amdgpu_device *a, bool restore, uint32_t vmid)
{ (void)a; (void)restore; (void)vmid; return 0; }
static uint32_t kgd_disable_debug_trap(struct amdgpu_device *a, bool keep, uint32_t vmid)
{ (void)a; (void)keep; (void)vmid; return 0; }
static const struct kfd2kgd_calls kfd2kgd = {
	.init_interrupts = kgd_init_interrupts,
	.enable_debug_trap = kgd_enable_debug_trap,
	.disable_debug_trap = kgd_disable_debug_trap,
};

void fixture_device_init(void)
{
	adev = calloc(1, sizeof(*adev));
	assert(adev);
	vram = calloc(1, TEST_VRAM_BYTES);
	assert(vram);
	adev->asic_type = CHIP_IP_DISCOVERY;
	adev->ip_versions[GC_HWIP][0] = IP_VERSION(12, 0, 1);
	adev->enable_mes = true;
	adev->mes.funcs = &fake_mes_funcs;
	mutex_init(&adev->mes.mutex_hidden);
	/* mes_v12_0: [0:3] hung doorbells, [4:7] HQD info. */
	adev->mes.hung_queue_db_array_size = 8;
	adev->mes.hung_queue_hqd_info_offset = 4;
	adev->mes.hung_queue_db_array_cpu_addr[0] = mes_hung_db_array;
	adev->reset_domain = &reset_domain;
	init_rwsem(&reset_domain.sem);
	reset_domain.wq = alloc_ordered_workqueue("test-reset", 0);
	assert(reset_domain.wq);
	adev->vm_manager.max_pfn = 1ULL << (48 - AMDGPU_GPU_PAGE_SHIFT);
	adev->gmc.vram_start = TEST_VRAM_START;
	adev->gmc.shared_aperture_start = 0x2000000000000000ULL;
	adev->gmc.shared_aperture_end = adev->gmc.shared_aperture_start + (4ULL << 30) - 1;
	adev->gmc.private_aperture_start = 0x1000000000000000ULL;
	adev->gmc.private_aperture_end = adev->gmc.private_aperture_start + (4ULL << 30) - 1;
	adev->doorbell.base = TEST_DOORBELL_BUS;
	adev->doorbell.size = TEST_DOORBELL_BYTES;
	adev->doorbell.cpu_addr = (u32 *)doorbell_bar;
	memset(doorbell_bar, 0xff, sizeof(doorbell_bar));
	adev->mman.buffer_funcs_enabled = true;
	adev->mman.buffer_funcs_ring = &sdma_ring;
	sdma_ring.sched.ready = true;
	mutex_init(&adev->mman.default_entity.lock);
	adev->ddev.render = &render_minor;
	adev->kfd.dev = &kfd;
	spin_lock_init(&sdma_lock);

	kfd.adev = adev;
	kfd.init_complete = true;
	kfd.num_nodes = 1;
	kfd.nodes[0] = &node;
	kfd.kfd2kgd = &kfd2kgd;
	kfd.device_info.doorbell_size = 8;
	kfd.device_info.num_sdma_queues_per_engine = 8;
	kfd.device_info.max_no_of_hqd = 24;
	kfd.shared_resources.enable_mes = true;
	kfd.shared_resources.num_pipe_per_mec = 4;
	kfd.shared_resources.num_queue_per_pipe = 8;
	kfd.shared_resources.gpuvm_size = 1ULL << 47;
	kfd.shared_resources.non_cp_doorbells_start = 0x180;
	kfd.shared_resources.non_cp_doorbells_end = 0x1ff;
	mutex_init(&kfd.doorbell_mutex);
	ida_init(&kfd.doorbell_ida);

	node.adev = adev;
	node.kfd = &kfd;
	node.kfd2kgd = &kfd2kgd;
	node.id = TEST_GPU_ID;
	node.xcc_mask = 1;
	node.vm_info.first_vmid_kfd = 8;
	node.vm_info.last_vmid_kfd = 15;
	node.vm_info.vmid_num_kfd = 8;
	node.compute_vmid_bitmap = 0xff00;
	node.max_proc_per_quantum = 8;
	node.dqm = device_queue_manager_init(&node);
	assert(node.dqm && node.dqm->sched_policy != KFD_SCHED_POLICY_NO_HWS);
	assert(!node.dqm->ops.start(node.dqm));

	topo.gpu_id = TEST_GPU_ID;
	topo.gpu = &node;
	topo.node_props.gfx_target_version = 120001;
	topo.node_props.simd_count = 128;
	topo.node_props.simd_per_cu = 2;
	topo.node_props.array_count = 8;
	topo.node_props.simd_arrays_per_engine = 2;
	topo.node_props.lds_size_in_kb = 64;
	kfd_queue_ctx_save_restore_size(&topo);
	assert(topo.node_props.cwsr_size && topo.node_props.ctl_stack_size &&
	       topo.node_props.eop_buffer_size);
}

void fixture_device_fini(void)
{
	device_queue_manager_uninit(node.dqm);
	node.dqm = NULL;
	ida_destroy(&kfd.doorbell_ida);
	destroy_workqueue(reset_domain.wq);
	free(vram);
	free(adev);
}


/* Kernel-free entry points the C++ tests use. */
struct amdgpu_device *fixture_adev(void) { return adev; }
struct rt_compute_ctx *fixture_compute_ctx(void) { return &compute_ctx; }
size_t fixture_kmalloc_live(void) { return kmemcheck_live_bytes(); }
unsigned int fixture_mes_adds(void) { return mes_adds; }
unsigned int fixture_mes_removes(void) { return mes_removes; }
unsigned int fixture_live_bos(void) { return live_bos; }
unsigned int fixture_kernel_allocs(void) { return kernel_allocs; }
unsigned int fixture_cp_dispatches(void) { return cp_dispatches; }
unsigned int fixture_render_balance(void) { return render_opens - render_releases; }
uint64_t fixture_doorbell(uint32_t index) { return doorbell_bar[index / 2]; }

void fixture_kfd_init(void)
{
	kfd_debugfs_init();
	assert(!kfd_chardev_init());
	assert(register_chrdev(DRM_MAJOR, "drm", &render_fops) == 0);
}
void fixture_kfd_wq_init(void) { assert(!kfd_process_create_wq()); }
void fixture_kfd_release_processes(void)
{
	kfd_cleanup_processes();
	kfd_process_destroy_wq();
	rcu_barrier();	/* kfree_rcu'd fences */
}
void fixture_kfd_exit(void)
{
	kfd_chardev_exit();
	unregister_chrdev(DRM_MAJOR, "drm");
	kfd_debugfs_fini();
}

/* Host bytes behind @va in the VM of the process with @pasid, contiguous
 * for @bytes (within one page for system memory). */
void *fixture_va_to_host(uint32_t pasid, uint64_t va, uint64_t bytes)
{
	struct amdgpu_vm *vm = NULL;

	for (unsigned int i = 0; i < ARRAY_SIZE(render_files); ++i)
		if (render_files[i] && render_files[i]->fpriv.vm.pasid == pasid)
			vm = &render_files[i]->fpriv.vm;
	if (!vm)
		return NULL;
	for (unsigned int i = 0; i < MAX_BOS; ++i) {
		struct fake_bo *f = mapped[i];
		uint64_t off;

		if (!f || f->vm != vm || va < f->mem.va || va - f->mem.va >= f->bo.tbo.base.size)
			continue;
		off = va - f->mem.va;
		if (bytes > f->bo.tbo.base.size - off)
			return NULL;
		if (f->pages) {
			if ((off & (PAGE_SIZE - 1)) + bytes > PAGE_SIZE)
				return NULL;
			return (char *)page_address(f->pages[off / PAGE_SIZE]) + (off & (PAGE_SIZE - 1));
		}
		return vram + amdgpu_vram_mgr_block_start(&f->block) + off;
	}
	return NULL;
}
/* The pasid a mapped VA belongs to (the first VM mapping it). */
uint32_t fixture_pasid_of(uint64_t va)
{
	for (unsigned int i = 0; i < MAX_BOS; ++i) {
		struct fake_bo *f = mapped[i];
		if (f && va >= f->mem.va && va - f->mem.va < f->bo.tbo.base.size)
			for (unsigned int j = 0; j < ARRAY_SIZE(render_files); ++j)
				if (render_files[j] && &render_files[j]->fpriv.vm == f->vm)
					return render_files[j]->fpriv.vm.pasid;
	}
	return 0;
}

/* amd_queue_t / AQL layout the CP reads (checked by the C++ test). */
static void cp_service(struct cp_queue *q)
{
	uint64_t *write, *read, ring, packets;
	char *queue;

	if (fixture_doorbell(q->doorbell) == UINT64_MAX)
		return;	/* never kicked */
	queue = fixture_va_to_host(q->pasid, q->wptr - FIXTURE_AQL_WRITE_ID, FIXTURE_AQL_QUEUE_BYTES);
	if (!queue)
		return;
	write = (uint64_t *)(queue + FIXTURE_AQL_WRITE_ID);
	read = (uint64_t *)(queue + FIXTURE_AQL_READ_ID);
	ring = *(uint64_t *)(queue + FIXTURE_AQL_RING_BASE);
	packets = *(uint32_t *)(queue + FIXTURE_AQL_RING_SIZE);
	while (packets && __atomic_load_n(read, __ATOMIC_ACQUIRE) < __atomic_load_n(write, __ATOMIC_ACQUIRE)) {
		uint64_t id = *read;
		char *packet = fixture_va_to_host(q->pasid, ring + (id % packets) * 64, 64);
		uint16_t header;

		if (!packet)
			return;
		header = __atomic_load_n((uint16_t *)packet, __ATOMIC_ACQUIRE);
		if ((header & 0xff) == FIXTURE_AQL_PACKET_INVALID)
			return;	/* not published yet */
		if ((header & 0xff) == FIXTURE_AQL_PACKET_DISPATCH) {
			uint64_t signal = *(uint64_t *)(packet + FIXTURE_AQL_COMPLETION);
			int64_t *value = signal ? fixture_va_to_host(q->pasid, signal + 8, 8) : NULL;

			if (value)
				__atomic_fetch_sub(value, 1, __ATOMIC_RELEASE);
			cp_dispatches++;
		}
		__atomic_store_n((uint16_t *)packet, FIXTURE_AQL_PACKET_INVALID, __ATOMIC_RELEASE);
		__atomic_store_n(read, id + 1, __ATOMIC_RELEASE);
	}
}
static void *cp_main(void *arg)
{
	(void)arg;
	while (cp_running) {
		pthread_mutex_lock(&cp_lock);
		for (unsigned int i = 0; i < ARRAY_SIZE(cp_queues); ++i)
			if (cp_queues[i].live)
				cp_service(&cp_queues[i]);
		pthread_mutex_unlock(&cp_lock);
		usleep(50);
	}
	return NULL;
}
void fixture_cp_start(void)
{
	cp_running = true;
	assert(!pthread_create(&cp_thread, NULL, cp_main, NULL));
}
void fixture_cp_stop(void)
{
	cp_running = false;
	pthread_join(cp_thread, NULL);
}

unsigned int fixture_report_bos(void)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < MAX_BOS; ++i)
		if (bos[i]) {
			fprintf(stderr, "kfd fixture: BO left: size %llu refs %d va %#llx gpu %#llx\n",
				(unsigned long long)bos[i]->bo.tbo.base.size, bos[i]->refs,
				(unsigned long long)bos[i]->mem.va,
				(unsigned long long)bos[i]->gpu_offset);
			n++;
		}
	return n;
}
