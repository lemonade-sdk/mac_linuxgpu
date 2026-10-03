/* Exercise alternate runtime/DMA-op entry points using the same mocked
 * DriverKit allocation fixture as the page/DART regression. */
#define main page_fixture_main
#include "test_page_alloc_dk.c"
#undef main
#include "../src/amdgpu-rt/device.c"
extern const struct dma_map_ops linuxu_dma_ops;
static bool stalled;
static bool test_stalled(void *arg) { return *(bool *)arg; }

int main(void)
{
    struct rt_device dev = {0};
    dev.pdev.dev.coherent_dma_mask = DMA_BIT_MASK(44);
    dma_addr_t runtime_dma, ops_dma, standard_dma;
    void *runtime = rt_dart_alloc(&dev, PAGE_SIZE, &runtime_dma);
    void *ops = linuxu_dma_ops.alloc(&dev.pdev.dev, PAGE_SIZE, &ops_dma, GFP_KERNEL, 0);
    void *standard = linuxu_dma_alloc_coherent(&dev.pdev.dev, PAGE_SIZE,
                                              &standard_dma, GFP_KERNEL);
    assert(runtime && ops && standard);
    assert(runtime_dma != (uintptr_t)runtime && ops_dma != (uintptr_t)ops);
    assert(rt_dart_used(&dev) == 3 * PAGE_SIZE);
    assert(rt_dart_ceiling(&dev) == linuxu_dart_budget());
    struct sg_table table = {0};
    assert(linuxu_dma_ops.get_sgtable(&dev.pdev.dev, &table, ops, ops_dma,
                                    PAGE_SIZE, 0) == -EOPNOTSUPP);
    assert(!table.sgl);
    assert(dma_mapping_error(&dev.pdev.dev,
        linuxu_dma_ops.map_phys(&dev.pdev.dev, 0x100000, PAGE_SIZE, DMA_TO_DEVICE, 0)));
    assert(dma_mapping_error(&dev.pdev.dev,
        dma_map_resource(&dev.pdev.dev, 0x100000, PAGE_SIZE, DMA_TO_DEVICE, 0)));
    fail_completion = 1;
    rt_dart_free(&dev, runtime, PAGE_SIZE, runtime_dma);
    linuxu_dma_ops.free(&dev.pdev.dev, PAGE_SIZE, ops, ops_dma, 0);
    linuxu_dma_free_coherent(&dev.pdev.dev, PAGE_SIZE, standard, standard_dma);
    assert(backing_live == 3 && rt_dart_used(&dev) == 3 * PAGE_SIZE);
    linuxu_dart_reset();
    assert(rt_dart_used(&dev) == 3 * PAGE_SIZE);
    fail_completion = 0;
    rt_dart_free(&dev, runtime, PAGE_SIZE, runtime_dma);
    linuxu_dma_ops.free(&dev.pdev.dev, PAGE_SIZE, ops, ops_dma, 0);
    linuxu_dma_free_coherent(&dev.pdev.dev, PAGE_SIZE, standard, standard_dma);
    assert(!backing_live && !rt_dart_used(&dev));

    /* A stalled engine holds DMA releases: the buffer stays mapped and
     * allocated (the DART still translates it) until no engine is stalled. */
    stalled = true;
    linuxu_dart_set_hold(test_stalled, &stalled);
    void *held = linuxu_dma_alloc_coherent(&dev.pdev.dev, PAGE_SIZE, &standard_dma, GFP_KERNEL);
    assert(held && backing_live == 1);
    linuxu_dma_free_coherent(&dev.pdev.dev, PAGE_SIZE, held, standard_dma);
    assert(backing_live == 1 && linuxu_dart_held() == 1);
    assert(linuxu_dart_contains(standard_dma, PAGE_SIZE));
    assert(linuxu_dart_release_held() == 1 && backing_live == 1);
    stalled = false;
    assert(linuxu_dart_release_held() == 0 && !backing_live && !rt_dart_used(&dev));
    assert(!linuxu_dart_contains(standard_dma, PAGE_SIZE));
    /* Removing the predicate releases what is held as well. */
    stalled = true;
    held = linuxu_dma_alloc_coherent(&dev.pdev.dev, PAGE_SIZE, &standard_dma, GFP_KERNEL);
    linuxu_dma_free_coherent(&dev.pdev.dev, PAGE_SIZE, held, standard_dma);
    assert(backing_live == 1 && linuxu_dart_held() == 1);
    linuxu_dart_set_hold(NULL, NULL);
    assert(!backing_live && !linuxu_dart_held() && !rt_dart_used(&dev));
    return 0;
}
