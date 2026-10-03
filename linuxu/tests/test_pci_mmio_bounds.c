/* Offline host MMIO allocation and resource-span failure checks. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/pci.h>
#include <rt/rt.h>

extern uint32_t rt_mmio_mint_token(struct pci_dev *, uint8_t, uint64_t, uint64_t, int);
extern void rt_mmio_free_token(uint32_t);
extern void *rt_mmio_iomap(struct pci_dev *, int, unsigned long);
extern void rt_mmio_iounmap(struct pci_dev *, void *);
static int fail_allocation;
static size_t requested_allocation;
void *pci_test_calloc(size_t n, size_t size)
{
    requested_allocation = n * size;
    if (fail_allocation) return NULL;
    return calloc(n, size);
}

int main(void)
{
    struct pci_dev pdev = {0};
    fail_allocation = 1;
    assert(!rt_mmio_mint_token(&pdev, 5, 0, 32, 1));
    assert(!rt_mmio_mint_token(&pdev, 5, 0, UINT64_MAX, 1));
    assert(requested_allocation == RT_MMIO_REGION_SIZE);
    fail_allocation = 0;
    uint32_t token = rt_mmio_mint_token(&pdev, 5, 0, 32, 1);
    assert(token == 1); /* Failed publication did not consume a slot. */
    void *mapping = (void *)(uintptr_t)token;
    rt_mmio_writeq(NULL, mapping, 1, 0x1122334455667788ULL);
    assert(rt_mmio_readq(NULL, mapping, 1) == 0x1122334455667788ULL);
    rt_mmio_writel(NULL, mapping, 28, 0x12345678);
    assert(rt_mmio_readl(NULL, mapping, 28) == 0x12345678);
    rt_mmio_writel(NULL, mapping, 29, UINT32_MAX);
    assert(rt_mmio_readl(NULL, mapping, 28) == 0x12345678);
    assert(!rt_mmio_readq(NULL, mapping, 25));
    assert(!rt_mmio_readq(NULL, mapping, UINT64_MAX - 3));
    rt_mmio_writeq(NULL, mapping, UINT64_MAX - 3, UINT64_MAX);
    uint8_t bytes[8] = {1,2,3,4,5,6,7,8}, output[8];
    rt_mmio_memcpy_toio(mapping, bytes, UINT64_MAX - 3, sizeof(bytes), NULL);
    memset(output, 0xff, sizeof(output));
    rt_mmio_memcpy_fromio(output, mapping, UINT64_MAX - 3, sizeof(output), NULL);
    for (unsigned i=0; i<sizeof(output); ++i) assert(!output[i]);
    rt_mmio_free_token(token);
    assert(!rt_mmio_readl(NULL, mapping, 0));

    pdev.resource[2].start = 0x10000;
    pdev.resource[2].end = 0x1001f;
    pdev.resource[2].flags = IORESOURCE_MEM;
    assert(!rt_mmio_iomap(NULL, 0, 0));
    assert(!rt_mmio_iomap(&pdev, -1, 0));
    assert(!rt_mmio_iomap(&pdev, 6, 0));
    mapping = rt_mmio_iomap(&pdev, 2, 4096);
    assert(mapping);
    rt_mmio_writel(NULL, mapping, 28, 0xabcd);
    assert(rt_mmio_readl(NULL, mapping, 28) == 0xabcd);
    rt_mmio_writel(NULL, mapping, 32, 0xaaaa);
    assert(!rt_mmio_readl(NULL, mapping, 32));
    rt_mmio_iounmap(&pdev, mapping);
    return 0;
}
