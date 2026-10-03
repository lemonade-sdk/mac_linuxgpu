/* The DriverKit MMIO branch with byte-access mocks and no device operations. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <linux/pci.h>
#include <rt/rt.h>
#include <rt/dext_pci.h>
extern uint32_t rt_mmio_mint_token(struct pci_dev *, uint8_t, uint64_t, uint64_t, int);
extern void rt_mmio_free_token(uint32_t);
static unsigned faults, reads, writes;
void dext_pci_transport_record_fault(int kind, uint64_t offset)
{ (void)kind; (void)offset; ++faults; }
int dext_mem_read8(uint32_t token, uint64_t offset, uint8_t *value)
{ assert(token && offset < 32); ++reads; *value = (uint8_t)offset; return 0; }
int dext_mem_write8(uint32_t token, uint64_t offset, uint8_t value)
{ assert(token && offset < 32); (void)value; ++writes; return 0; }
int main(void)
{
    struct pci_dev dev = {0};
    uint32_t token = rt_mmio_mint_token(&dev, 5, 0, 32, 0);
    assert(token);
    void *mapping = (void *)(uintptr_t)token;
    uint8_t bytes[8] = {0};
    rt_mmio_memcpy_fromio(bytes, mapping, 24, sizeof(bytes), NULL);
    assert(reads == 8 && bytes[0] == 24 && bytes[7] == 31);
    rt_mmio_memcpy_toio(mapping, bytes, 24, sizeof(bytes), NULL);
    assert(writes == 8 && faults == 0);
    rt_mmio_memcpy_fromio(bytes, mapping, 25, sizeof(bytes), NULL);
    for (unsigned i=0; i<sizeof(bytes); ++i) assert(bytes[i] == 0xff);
    rt_mmio_memcpy_toio(mapping, bytes, UINT64_MAX - 3, sizeof(bytes), NULL);
    assert(reads == 8 && writes == 8 && faults == 2);
    rt_mmio_free_token(token);
    rt_mmio_memcpy_toio(mapping, bytes, 0, sizeof(bytes), NULL);
    assert(writes == 8 && faults == 3);
    return 0;
}
