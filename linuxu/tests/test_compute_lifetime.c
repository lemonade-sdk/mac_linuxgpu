/* Exercise the production compute adapter using CPU-only BO/DMA boundaries. */
#include <assert.h>
#include <stdio.h>
#include "../src/amdgpu-rt/compute.c"
#include <drm/ttm/ttm_tt.h>
extern int usleep(unsigned int);

struct mock_bo { struct amdgpu_bo *bo; uint64_t address; size_t size; void *cpu; int vram; };
static struct mock_bo records[16];
static unsigned copies, live, dma_live;
static int fail_cleanup, timeout_copy;
static struct amdgpu_device device;
static struct pci_dev pci;
static struct amdgpu_ring ring;
static struct amdgpu_buffer_funcs buffer_funcs;

void msleep(unsigned int ms) { usleep(ms * 1000); }
ktime_t ktime_get(void) { return 0; }
/* rt/removal.h: whether the device left the bus. */
static bool removed;
bool rt_removal_active(struct amdgpu_device *adev) { (void)adev; return removed; }
/* The session's DMA hold: attached by open, detached by close. */
static bool (*hold_fn)(void *);
static void *hold_arg;
void linuxu_dart_set_hold(bool (*stalled)(void *), void *arg) { hold_fn = stalled; hold_arg = arg; }
int rt_pci_probe_result(struct pci_dev *pdev) { return pdev == &pci ? 0 : -ENODEV; }
int dma_resv_lock(struct dma_resv *r, struct ww_acquire_ctx *c)
{ (void)r; (void)c; return fail_cleanup && copies == 2 ? -EINTR : 0; }
int dma_resv_lock_interruptible(struct dma_resv *r, struct ww_acquire_ctx *c)
{ return dma_resv_lock(r, c); }
void dma_resv_unlock(struct dma_resv *r) { (void)r; }
void ttm_bo_move_to_lru_tail(struct ttm_buffer_object *bo) { (void)bo; }
int amdgpu_bo_create_kernel(struct amdgpu_device *adev, unsigned long size,
        int alignment, u32 domain, struct amdgpu_bo **out, u64 *gpu, void **cpu)
{
	(void)alignment; (void)cpu;
	unsigned i;
	for (i = 0; i < 16 && records[i].bo; ++i) {}
	assert(i < 16);
	struct mock_bo *rec = &records[i];
	rec->bo = calloc(1, sizeof(*rec->bo));
	assert(rec->bo);
	rec->bo->tbo.bdev = &adev->mman.bdev;
	rec->bo->tbo.resource = calloc(1, sizeof(*rec->bo->tbo.resource));
	rec->bo->tbo.ttm = calloc(1, sizeof(*rec->bo->tbo.ttm));
	assert(rec->bo->tbo.resource && rec->bo->tbo.ttm);
	rec->vram = domain == AMDGPU_GEM_DOMAIN_VRAM;
	rec->bo->tbo.resource->mem_type = rec->vram ? TTM_PL_VRAM : TTM_PL_TT;
	rec->address = (rec->vram ? 0x100000000ull : adev->gmc.gart_start) + i * 65536;
	rec->size = size;
	if (rec->vram) rec->cpu = calloc(1, size);
	*out = rec->bo; *gpu = rec->address; live++;
	return 0;
}
void amdgpu_bo_free_kernel(struct amdgpu_bo **bo, u64 *gpu, void **cpu)
{
	(void)cpu;
	for (unsigned i = 0; i < 16; ++i) {
		struct mock_bo *rec = &records[i];
		if (rec->bo != *bo) continue;
		if (rec->vram) free(rec->cpu);
		free(rec->bo->tbo.resource); free(rec->bo->tbo.ttm); free(rec->bo);
		memset(rec, 0, sizeof(*rec)); *bo = NULL; *gpu = 0; live--; return;
	}
	assert(0);
}
void *linuxu_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma, gfp_t gfp)
{
	(void)dev; (void)gfp;
	void *cpu = calloc(1, size); assert(cpu); *dma = (uintptr_t)cpu; dma_live++; return cpu;
}
void linuxu_dma_free_coherent(struct device *dev, size_t size, void *cpu, dma_addr_t dma)
{ (void)dev; (void)size; assert((uintptr_t)cpu == dma); free(cpu); dma_live--; }
uint64_t amdgpu_ttm_tt_pte_flags(struct amdgpu_device *adev, struct ttm_tt *ttm, struct ttm_resource *mem)
{ (void)adev; (void)ttm; (void)mem; return 0; }
void amdgpu_gart_bind(struct amdgpu_device *adev, uint64_t offset, int pages, dma_addr_t *dma, uint64_t flags)
{
	(void)pages; (void)flags;
	for (unsigned i = 0; i < 16; ++i)
		if (records[i].bo && records[i].address == adev->gmc.gart_start + offset)
			{ records[i].cpu = (void *)(uintptr_t)*dma; return; }
	assert(0);
}
void amdgpu_gart_unbind(struct amdgpu_device *adev, uint64_t offset, int pages)
{ (void)adev; (void)offset; (void)pages; }
void amdgpu_gart_invalidate_tlb(struct amdgpu_device *adev) { (void)adev; }
static void *gpu_pointer(uint64_t address, uint32_t bytes)
{
	for (unsigned i = 0; i < 16; ++i) {
		struct mock_bo *r = &records[i];
		if (r->bo && address >= r->address && address - r->address <= r->size &&
		    bytes <= r->size - (address - r->address))
			return (char *)r->cpu + address - r->address;
	}
	assert(0); return NULL;
}
int amdgpu_copy_buffer(struct amdgpu_device *adev, struct amdgpu_ttm_buffer_entity *entity,
        uint64_t src, uint64_t dst, uint32_t bytes, struct dma_resv *resv,
        struct dma_fence **fence, bool vm_flush, uint32_t flags)
{
	(void)adev; (void)entity; (void)resv; (void)vm_flush; (void)flags;
	memcpy(gpu_pointer(dst, bytes), gpu_pointer(src, bytes), bytes);
	*fence = calloc(1, sizeof(**fence)); assert(*fence); kref_init(&(*fence)->refcount);
	copies++; return 0;
}
long dma_fence_wait_timeout(struct dma_fence *fence, bool intr, long timeout)
{ (void)fence; (void)intr; (void)timeout; return timeout_copy ? 0 : 1; }
int dma_fence_get_status(struct dma_fence *fence) { (void)fence; return 1; }
void dma_fence_release(struct kref *ref) { free(container_of(ref, struct dma_fence, refcount)); }

int main(void)
{
	struct rt_compute_ctx *ctx;
	struct rt_compute_bo *a, *b;
	struct rt_compute_fence *fence;
	char bytes[16] = {0x44}, readback[16] = {0};
	device.pdev = &pci; pci_set_drvdata(&pci, &device.ddev);
	device.accel_working = device.mman.initialized = true;
	device.gart.ptr = (void *)&device; device.gart.bo = (void *)&device;
	device.gmc.gart_start = 0x100000; device.gmc.gart_size = 1024 * 1024;
	device.gmc.real_vram_size = 1024 * 1024 * 16;
	device.mman.buffer_funcs_enabled = true; device.mman.buffer_funcs = &buffer_funcs;
	device.mman.buffer_funcs_ring = &ring; ring.sched.ready = true;
	mutex_init(&device.mman.default_entity.lock);
	assert(rt_compute_open(&pci, &ctx) == 0);
	/* No ring has a job: no engine is stalled. */
	assert(hold_fn && hold_arg == &device && !hold_fn(hold_arg));
	assert(rt_compute_verify_host_memory(ctx) == 0);
	assert(rt_compute_status(ctx) == 0 && live == 0 && dma_live == 0 && copies == 2);
	assert(rt_compute_close(ctx) == 0);
	assert(!hold_fn && !hold_arg);

	copies = 0; fail_cleanup = 1;
	assert(rt_compute_open(&pci, &ctx) == 0);
	assert(rt_compute_verify_host_memory(ctx) != 0);
	assert(rt_compute_status(ctx) != 0 && !ctx->host_memory_verified);
	assert(live == 2 && dma_live == 2); /* failed cleanup keeps both mappings */
	fail_cleanup = 0;
	assert(rt_compute_close(ctx) == 0 && live == 0 && dma_live == 0);

	assert(rt_compute_open(&pci, &ctx) == 0);
	assert(rt_compute_bo_alloc(ctx, PAGE_SIZE, PAGE_SIZE, RT_COMPUTE_GTT, &a) == 0);
	assert(rt_compute_bo_alloc(ctx, PAGE_SIZE, PAGE_SIZE, RT_COMPUTE_GTT, &b) == 0);
	assert(rt_compute_bo_write(ctx, a, 0, bytes, sizeof(bytes)) == 0);
	timeout_copy = 1;
	assert(rt_compute_bo_copy(ctx, a, b, 0, 0, sizeof(bytes), &fence) == -ETIMEDOUT);
	assert(!fence && ctx->poisoned);
	assert(rt_compute_bo_write(ctx, a, 0, readback, sizeof(readback)) == -EBUSY);
	assert(rt_compute_bo_read(ctx, a, 0, readback, sizeof(readback)) == -EBUSY);
	assert(rt_compute_bo_free(ctx, a) == -EBUSY);
	assert(rt_compute_close(ctx) == -EBUSY && live == 2 && dma_live == 2);
	/* The device leaves the bus: nothing can reach the memory a timed-out
	 * copy used, so the context releases it. */
	removed = true;
	assert(rt_compute_bo_free(ctx, a) == 0 && live == 1 && dma_live == 1);
	assert(rt_compute_close(ctx) == 0 && live == 0 && dma_live == 0);
	removed = false;
	puts("production compute verification cleanup and timeout retention passed");
	return 0;
}
