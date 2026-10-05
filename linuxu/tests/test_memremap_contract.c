/* Normal-pointer memremap users must receive a real CPU aperture, never a
 * fabricated heap buffer or synthetic register token. No hardware access. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <linux/io.h>

static unsigned char aperture[64];
static unsigned maps, unmaps;
static void *last_unmap;
static const uint64_t bar0 = 0x80000000;
static const uint64_t bar5 = 0x90000000;

void *rt_ioremap_active(uint64_t start, uint64_t size)
{
    ++maps;
    if (start >= bar0 && start - bar0 < sizeof(aperture) &&
        size <= sizeof(aperture) - (start - bar0))
        return aperture + (start - bar0);
    if (start == bar5 && size == 4)
        return (void *)(uintptr_t)rt_mmio_dk_address(1);
    return NULL;
}
int dext_bar0_cpu_contains(const void *address, size_t size)
{
    uintptr_t offset = (uintptr_t)address - (uintptr_t)aperture;
    return offset < sizeof(aperture) && size <= sizeof(aperture) - offset;
}
void rt_mmio_free(void *address) { ++unmaps; last_unmap = address; }

int main(void)
{
    void *mapped = memremap(bar0 + 8, 16, MEMREMAP_WB | MEMREMAP_WT | MEMREMAP_WC);
    assert(mapped == aperture + 8 && maps == 1);
    memset(mapped, 0x5a, 16);
    assert(aperture[8] == 0x5a && aperture[23] == 0x5a);
    memunmap(mapped);
    assert(unmaps == 1 && last_unmap == mapped);
    assert(!memremap(bar5, 4, MEMREMAP_WC));
    assert(unmaps == 2 && last_unmap == (void *)(uintptr_t)rt_mmio_dk_address(1));
    assert(!memremap(bar0 + 60, 8, MEMREMAP_WB));
    assert(!memremap(UINT64_MAX, 16, MEMREMAP_WC));
    unsigned before = maps;
    assert(!memremap(bar0, 0, MEMREMAP_WC));
    assert(!memremap(bar0, 4, 0));
    assert(!memremap(bar0, 4, 1UL << 63));
    assert(maps == before);
    memunmap(NULL);
    assert(unmaps == 2);
    return 0;
}
