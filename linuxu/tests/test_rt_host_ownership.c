/* Host-only allocator failure, concurrent arena init, and ownership checks. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/slab.h>
#include <rt/amdgpu_device_ext.h>

static int fail_vram = 1;
static unsigned arena_allocations;
static const char *arena_size = "1";
static const char *rt_test_getenv(const char *name) { (void)name; return arena_size; }
static void *rt_test_aligned_alloc(size_t align, size_t size)
{
    ++arena_allocations;
    return fail_vram ? NULL : aligned_alloc(align, size);
}
#define getenv rt_test_getenv
#define aligned_alloc rt_test_aligned_alloc
#include "../src/amdgpu-rt/vram.c"
#undef aligned_alloc
#undef getenv

static unsigned bo_backings;
void *rt_test_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (p) ++bo_backings;
    return p;
}
void rt_test_free(void *p)
{
    if (p) { assert(bo_backings); --bo_backings; }
    free(p);
}
void *kzalloc(size_t size, gfp_t flags) { (void)flags; return calloc(1, size); }
void *kcalloc(size_t n, size_t size, gfp_t flags) { (void)flags; return calloc(n, size); }
void kfree(const void *p) { free((void *)p); }

struct linuxu_ttm_bo_shim;
extern struct linuxu_ttm_bo_shim *linuxu_ttm_bo_shim_create(int);
extern int linuxu_ttm_bo_shim_mmap(struct linuxu_ttm_bo_shim *, void **);
extern void linuxu_ttm_bo_shim_unmap(struct linuxu_ttm_bo_shim *);
extern void linuxu_ttm_bo_shim_destroy(struct linuxu_ttm_bo_shim *);

static void *allocate_thread(void *arg)
{
    void **result = arg;
    *result = linuxu_vram_alloc(1);
    return NULL;
}

int main(void)
{
    assert(!vram_pfn_to_ptr(1)); /* OOM must never manufacture a low pointer. */
    assert(!linuxu_vram_size());
    assert(!linuxu_vram_alloc(1));
    arena_size = "18446744073709551615";
    unsigned before = arena_allocations;
    assert(!linuxu_vram_size() && arena_allocations == before);
    arena_size = "1";
    fail_vram = 0;
    pthread_t threads[8];
    void *addresses[8];
    before = arena_allocations;
    for (unsigned i=0; i<8; ++i)
        assert(!pthread_create(&threads[i], NULL, allocate_thread, &addresses[i]));
    for (unsigned i=0; i<8; ++i) {
        assert(!pthread_join(threads[i], NULL));
        assert(addresses[i]);
        for (unsigned j=0; j<i; ++j) assert(addresses[i] != addresses[j]);
    }
    assert(arena_allocations == before + 1);
    assert(linuxu_vram_used() == 8 * VRAM_PAGE_SIZE);
    assert(!linuxu_vram_alloc(SIZE_MAX));
    assert(!linuxu_vram_alloc(0));
    assert(!vram_pfn_to_ptr(UINT32_MAX));
    assert(vram_ptr_to_pfn(&before) == UINT32_MAX);
    assert(vram_pfn_to_ptr(vram_ptr_to_pfn(addresses[0])) == addresses[0]);

    struct amdgpu_device_ext *ext[8];
    assert(!rt_amdgpu_device_alloc(NULL));
    for (uintptr_t i=0; i<8; ++i) {
        struct amdgpu_device *adev = (void *)(i + 1);
        ext[i] = rt_amdgpu_device_alloc(adev);
        assert(ext[i] && rt_amdgpu_device_ext_get(adev) == ext[i]);
        assert(!rt_amdgpu_device_alloc(adev));
    }
    assert(!rt_amdgpu_device_alloc((void *)9));
    for (uintptr_t i=0; i<8; ++i) {
        rt_amdgpu_device_free(ext[i]);
        assert(!rt_amdgpu_device_ext_get((void *)(i + 1)));
    }

    struct linuxu_ttm_bo_shim *bo = linuxu_ttm_bo_shim_create(1);
    assert(bo);
    void *first, *second;
    assert(!linuxu_ttm_bo_shim_mmap(bo, &first) && bo_backings == 1);
    memset(first, 0x7a, 16384);
    linuxu_ttm_bo_shim_unmap(bo);
    assert(!linuxu_ttm_bo_shim_mmap(bo, &second));
    assert(first == second && bo_backings == 1 && *(uint8_t *)second == 0x7a);
    linuxu_ttm_bo_shim_destroy(bo);
    assert(bo_backings == 0);
    return 0;
}
