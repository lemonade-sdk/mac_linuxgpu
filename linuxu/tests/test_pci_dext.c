/* Mock DriverKit config backend; no PCI hardware access.
 * Build: clang -w -std=gnu11 -DLINUXU_DEXT_DK=1 -ffunction-sections
 *   -fdata-sections -Ilinuxu/headers -Ithird_party/linux/drivers/gpu/drm/amd/include
 *   linuxu/tests/test_pci_dext.c linuxu/src/pci/pci_stub.c
 *   linuxu/src/dart/dma_mask.c
 *   -Wl,-dead_strip -o /tmp/test_pci_dext && /tmp/test_pci_dext
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <linux/pci.h>
#include <rt/dext_pci.h>

#ifdef LINUXU_TEST_UPSTREAM_BUS_STATUS
/* This is the exact upstream health check, with only its device layout
 * reduced to the fields it accesses. Reads use the production PCI shim. */
struct amdgpu_device { struct pci_dev *pdev; struct device *dev; };
#include "upstream_bus_status.inc"
#endif

static uint8_t cfg[4096];
static int fail_read_at = -1;
static int fail_write_at = -1;
static int suppress_write_at = -1;
static unsigned int config_writes;
static unsigned int faults, reset_calls;
static int fail_state_alloc;
void *kmalloc(size_t size, gfp_t flags) { (void)flags; return fail_state_alloc ? NULL : malloc(size); }
void kfree(const void *p) { free((void *)p); }
void dext_pci_transport_record_fault(int fault, uint64_t offset)
{ (void)fault; (void)offset; ++faults; }
int dext_pci_function_reset(void) { ++reset_calls; return -95; }
/* DMA seam: a platform whose DART places mappings above 1 TiB, so it
 * serves 41-bit and wider device masks only. */
static unsigned int dma_width;
int dext_dma_platform_supports_bits(unsigned int bits) { return bits >= 41; }
int dext_dma_set_address_bits(unsigned int bits) { dma_width = bits; return 0; }
static unsigned int armed_irq_vectors;
static unsigned int actual_irq_type = DEXT_PCI_IRQ_NONE;
static uint16_t rd16(unsigned off) { return cfg[off] | ((uint16_t)cfg[off+1] << 8); }
static uint32_t rd32(unsigned off) { return rd16(off) | ((uint32_t)rd16(off+2) << 16); }
static void wr16(unsigned off, uint16_t v) { cfg[off]=(uint8_t)v; cfg[off+1]=(uint8_t)(v>>8); }
static void wr32(unsigned off, uint32_t v) { wr16(off,(uint16_t)v); wr16(off+2,(uint16_t)(v>>16)); }
int dext_pci_config_read8(uint64_t o, uint8_t *v) { if(o>=sizeof(cfg) || (int)o==fail_read_at)return -1; *v=cfg[o]; return 0; }
int dext_pci_config_read16(uint64_t o, uint16_t *v) { if(o+1>=sizeof(cfg) || (int)o==fail_read_at)return -1; *v=rd16(o); return 0; }
int dext_pci_config_read32(uint64_t o, uint32_t *v) { if(o+3>=sizeof(cfg) || (int)o==fail_read_at)return -1; *v=rd32(o); return 0; }
int dext_pci_config_write8(uint64_t o, uint8_t v) { ++config_writes; if(o>=sizeof(cfg) || (int)o==fail_write_at)return -1; if((int)o!=suppress_write_at)cfg[o]=v; return 0; }
int dext_pci_config_write16(uint64_t o, uint16_t v) { ++config_writes; if(o+1>=sizeof(cfg) || (int)o==fail_write_at)return -1; if((int)o!=suppress_write_at)wr16(o,v); return 0; }
int dext_pci_config_write32(uint64_t o, uint32_t v) { ++config_writes; if(o+3>=sizeof(cfg) || (int)o==fail_write_at)return -1; if((int)o!=suppress_write_at)wr32(o,v); return 0; }
void msleep(unsigned int ms) { (void)ms; }
int dext_pci_irq_status(unsigned int *armed, unsigned int *type) {
    *armed = armed_irq_vectors;
    *type = actual_irq_type;
    return 0;
}

static void expect_invalid_power_config(struct pci_dev *p)
{
    unsigned int before = config_writes;
    assert(pci_enable_device(p) < 0 && p->enable_cnt == 0);
    assert(pci_power_state(p) < 0);
    assert(config_writes == before);
    assert(!(rd16(PCI_COMMAND) & PCI_COMMAND_MEMORY));
}

int main(void) {
    struct pci_dev p = {0};
    struct pci_bus bus = { .number = 5 };
    p.bus = &bus;
    p.devfn = PCI_DEVFN(0, 0);
    p.device = 0x744c;
    assert(pci_dev_id(&p) == 0x0500);
    p.devfn = PCI_DEVFN(31, 7);
    assert(pci_dev_id(&p) == 0x05ff);
    _Static_assert(PCI_VENDOR_ID == 0, "vendor register offset");
#ifdef LINUXU_TEST_UPSTREAM_BUS_STATUS
    struct amdgpu_device adev = { .pdev = &p, .dev = &p.dev };
    wr32(PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    assert(amdgpu_device_bus_status_check(&adev) == 0);
    wr32(PCI_COMMAND, 0x00100406); /* nonzero status plus normal enables */
    assert(amdgpu_device_bus_status_check(&adev) == 0);
    wr32(PCI_COMMAND, UINT32_MAX);
    assert(amdgpu_device_bus_status_check(&adev) == -ENODEV);
    fail_read_at = PCI_COMMAND;
    assert(amdgpu_device_bus_status_check(&adev) == -ENODEV);
    fail_read_at = -1;
    wr32(PCI_COMMAND, 0);
    assert(amdgpu_device_bus_status_check(&adev) == 0);
#endif
    assert(PCI_POSSIBLE_ERROR((uint8_t)0xff));
    assert(PCI_POSSIBLE_ERROR((uint16_t)0xffff));
    assert(PCI_POSSIBLE_ERROR(UINT64_MAX));
    assert(!PCI_POSSIBLE_ERROR((uint16_t)0xff));
    wr16(0, 0x1002);
    u16 vendor = 0;
    assert(pci_read_config_word(&p, PCI_VENDOR_ID, &vendor) == 0);
    assert(vendor == 0x1002);
    memset(cfg, 0, sizeof(cfg));
    u64 dma_mask = DMA_BIT_MASK(44);
    /* Upstream amdgpu_prefer_rom_resource reads slot six even on a GPU
     * whose platform did not expose a ROM BAR. It must be a real resource. */
    assert(DEVICE_COUNT_RESOURCE > PCI_ROM_RESOURCE);
    p.resource[PCI_ROM_RESOURCE].flags = IORESOURCE_ROM_SHADOW;
    assert(p.dev.dma_mask == NULL && p.driver_data == NULL);
    p.resource[PCI_ROM_RESOURCE].flags = 0;
    assert(pci_rebar_bytes_to_size(1) == 0);
    assert(pci_rebar_bytes_to_size(1ULL << 20) == 0);
    assert(pci_rebar_bytes_to_size((1ULL << 20) + 1) == 1);
    assert(pci_rebar_bytes_to_size(256ULL << 20) == 8);
    assert(pci_rebar_bytes_to_size(32ULL << 30) == 15);
    assert(pci_rebar_bytes_to_size((32ULL << 30) + 1) == 16);
    assert(pci_rebar_bytes_to_size(UINT64_MAX) == 44);
    p.dev.dma_mask = &dma_mask;
    p.dev.coherent_dma_mask = dma_mask;
    p.dev.bus_dma_limit = dma_mask;
    p.cfg_size = sizeof(cfg);
    p.resource[0].flags = IORESOURCE_MEM;
    p.resource[5].flags = IORESOURCE_MEM;
    wr16(PCI_STATUS, PCI_STATUS_CAP_LIST);
    cfg[PCI_CAPABILITY_LIST] = 0x40;
    cfg[0x40] = PCI_CAP_ID_PM; cfg[0x41] = 0x50;
    wr16(0x40 + PCI_PM_CTRL, PCI_D3hot);
    cfg[0x50] = PCI_CAP_ID_MSIX; cfg[0x51] = 0;
    wr32(0x100, 1 | (1u << 16) | (0x140u << 20));
    wr32(0x140, 0x15 | (1u << 16));
    assert(pci_find_capability(&p, PCI_CAP_ID_PM) == 0x40);
    assert(pci_find_capability(&p, PCI_CAP_ID_MSIX) == 0x50);
    assert(pci_find_capability(&p, 0x100 + PCI_CAP_ID_PM) == 0);
    cfg[0x51] = 0x40; /* a malformed cycle must terminate */
    assert(pci_find_capability(&p, 0x7f) == 0);
    cfg[0x51] = 0;
    assert(pci_find_ext_capability(&p, 0x15) == 0x140);
    wr32(0x140, 0x15 | (1u << 16) | (0x100u << 20));
    assert(pci_find_ext_capability(&p, 0x7f) == 0);
    wr32(0x140, 0x15 | (1u << 16));
    /* Missing PM is valid only after a successful, well-formed lookup. */
    const unsigned read_failures[] = { PCI_STATUS, PCI_CAPABILITY_LIST,
        0x40, 0x41, 0x50, 0x51, 0x40 + PCI_PM_CTRL };
    for (unsigned i=0; i<sizeof(read_failures)/sizeof(read_failures[0]); ++i) {
        fail_read_at = read_failures[i];
        expect_invalid_power_config(&p);
    }
    fail_read_at = -1;
    const uint8_t bad_pointers[] = { 0x20, 0x43, 0xff };
    for (unsigned i=0; i<sizeof(bad_pointers); ++i) {
        cfg[PCI_CAPABILITY_LIST] = bad_pointers[i];
        expect_invalid_power_config(&p);
    }
    cfg[PCI_CAPABILITY_LIST] = 0x40;
    cfg[0x51] = 0x40; /* Even a matching PM node cannot hide a cyclic tail. */
    expect_invalid_power_config(&p);
    cfg[0x51] = 0x43;
    expect_invalid_power_config(&p);
    cfg[0x51] = 0;
    cfg[0x50] = UINT8_MAX;
    expect_invalid_power_config(&p);
    cfg[0x50] = PCI_CAP_ID_MSIX;
    wr16(PCI_STATUS, UINT16_MAX);
    expect_invalid_power_config(&p);
    wr16(PCI_STATUS, 0);
    assert(pci_power_state(&p) == PCI_D0);
    assert(pci_enable_device(&p) == 0); /* Capability list genuinely absent. */
    pci_disable_device(&p);
    wr16(PCI_STATUS, PCI_STATUS_CAP_LIST);
    cfg[0x40] = PCI_CAP_ID_MSI; /* A valid list without a PM capability. */
    assert(pci_power_state(&p) == PCI_D0);
    assert(pci_enable_device(&p) == 0);
    pci_disable_device(&p);
    cfg[0x40] = PCI_CAP_ID_PM;
    assert(pci_enable_device(&p) == 0);
    assert(p.enable_cnt == 1);
    assert((rd16(PCI_COMMAND) & 6) == PCI_COMMAND_MEMORY);
    assert((rd16(0x44) & 3) == PCI_D0);
    assert(pci_mmio_enabled(&p));
    assert(pci_enable_device(&p) == 0);
    assert(p.enable_cnt == 2);
    assert(pci_enable_bus_master(&p) == 0);
    assert((rd16(PCI_COMMAND) & PCI_COMMAND_MASTER) != 0);
    pci_clear_master(&p);
    assert((rd16(PCI_COMMAND) & PCI_COMMAND_MASTER) == 0);
    pci_disable_device(&p);
    assert(p.enable_cnt == 1 && pci_mmio_enabled(&p));
    pci_disable_device(&p);
    assert((rd16(PCI_COMMAND) & PCI_COMMAND_MEMORY) == 0);
    assert(!pci_mmio_enabled(&p));
    fail_read_at = PCI_COMMAND;
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    fail_read_at = -1;
    fail_write_at = PCI_COMMAND;
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    fail_write_at = -1;
    suppress_write_at = PCI_COMMAND;
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    suppress_write_at = -1;
    assert(pci_enable_device(&p) == 0);
    pci_disable_device(&p);
    wr16(PCI_COMMAND, UINT16_MAX);
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    assert(pci_enable_bus_master(&p) < 0);
    wr16(PCI_COMMAND, 0);
    wr16(0x40 + PCI_PM_CTRL, UINT16_MAX);
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    wr16(0x40 + PCI_PM_CTRL, PCI_D0);
    cfg[PCI_CAPABILITY_LIST] = 0xfc;
    cfg[0xfc] = PCI_CAP_ID_PM; cfg[0xfd] = 0;
    assert(pci_enable_device(&p) < 0 && p.enable_cnt == 0);
    cfg[PCI_CAPABILITY_LIST] = 0x40;
    /* A capability-relative read must not return an unrelated register
     * at the same absolute offset. Both v1 absence and faults are visible. */
    cfg[0x51] = 0x80;
    cfg[0x80] = PCI_CAP_ID_EXP; cfg[0x81] = 0;
    wr16(0x82, 2);
    p.is_pcie_device = 1;
    assert(pci_pcie_type(&p) == PCI_EXP_TYPE_ENDPOINT);
    wr16(0x82, 2 | (PCI_EXP_TYPE_DOWNSTREAM << 4));
    assert(pci_pcie_type(&p) == PCI_EXP_TYPE_DOWNSTREAM);
    wr16(0x82, 2);
    assert(!pcie_aspm_enabled(&p));
    wr32(0x8c, 5 | (16 << 4));
    assert(pcie_get_speed_cap(&p) == PCIE_SPEED_32_0GT);
    assert(pcie_get_width_cap(&p) == PCIE_LNK_WIDTH_X16);
    wr16(0x88, 3 << 5);
    assert(pcie_get_mps(&p) == 1024);
    wr16(0x88, 7 << 5);
    assert(pcie_get_mps(&p) == -EINVAL);
    wr32(0x8c, UINT32_MAX);
    assert(pcie_get_speed_cap(&p) == PCI_SPEED_UNKNOWN);
    assert(pcie_get_width_cap(&p) == PCIE_LNK_WIDTH_UNKNOWN);
    fail_read_at = 0x8c;
    assert(pcie_get_speed_cap(&p) == PCI_SPEED_UNKNOWN);
    assert(pcie_get_width_cap(&p) == PCIE_LNK_WIDTH_UNKNOWN);
    fail_read_at = -1;
    enum pci_bus_speed path_speed = PCIE_SPEED_32_0GT;
    enum pcie_link_width path_width = PCIE_LNK_WIDTH_X16;
    struct pci_dev *limiting = &p;
    assert(pcie_bandwidth_available(&p, &limiting, &path_speed, &path_width) == 0);
    assert(!limiting && path_speed == PCI_SPEED_UNKNOWN && path_width == PCIE_LNK_WIDTH_UNKNOWN);
    /* The upstream partner: none until the dext supplies one, which is
     * amdgpu's "platform speed unknown" (Gen1/Gen2 only). Once supplied it
     * answers from the registry values and never from the endpoint's
     * configuration space, which here says 32 GT/s x16. */
    assert(pci_upstream_bridge(&p) == NULL);
    wr32(0x8c, 5 | (16 << 4));
    linuxu_pci_set_upstream_partner(0x8086, 0x5786, 2 | (PCI_EXP_TYPE_DOWNSTREAM << 4),
                                    0x00715844); /* 16 GT/s x4, as an Intel TB5 port */
    struct pci_dev *partner = pci_upstream_bridge(&p);
    assert(partner && partner != &p && partner->vendor == 0x8086 && partner->device == 0x5786);
    assert(pci_upstream_bridge(partner) == NULL);
    assert(pcie_get_speed_cap(partner) == PCIE_SPEED_16_0GT);
    assert(pcie_get_width_cap(partner) == PCIE_LNK_WIDTH_X4);
    assert(pci_pcie_type(partner) == PCI_EXP_TYPE_DOWNSTREAM);
    {
        u32 value = 0;
        u16 half = 0;
        assert(pci_read_config_dword(partner, 0, &value) == -EINVAL && value == UINT32_MAX);
        assert(pci_write_config_dword(partner, 4, 0) == -EINVAL);
        assert(pcie_capability_read_word(partner, 0x12, &half) == -EINVAL);
    }
    assert(pcie_get_speed_cap(&p) == PCIE_SPEED_32_0GT);
    linuxu_pci_clear_upstream_partner();
    assert(pci_upstream_bridge(&p) == NULL);
    wr16(0x8a, 0x1234);
    wr32(0x8c, 0x12345678);
    wr16(0xa8, 0x4321);
    u16 word = 0;
    u32 dword = 0;
    assert(!pcie_capability_read_word(&p, 0x0a, &word) && word == 0x1234);
    assert(!pcie_capability_read_dword(&p, 0x0c, &dword) && dword == 0x12345678);
    assert(!pcie_capability_read_word(&p, 0x28, &word) && word == 0x4321);
    wr16(0x82, 1);
    assert(!pcie_capability_read_word(&p, 0x28, &word) && word == 0);
    fail_read_at = 0x8a;
    assert(pcie_capability_read_word(&p, 0x0a, &word) < 0);
    fail_read_at = 0x81;
    assert(pcie_capability_read_dword(&p, 0x0c, &dword) < 0);
    fail_read_at = -1;
    assert(pcie_capability_read_word(&p, 1, &word) < 0);
    assert(pcie_capability_read_dword(&p, 0x3c, &dword) < 0);
    assert(pcie_capability_read_dword(&p, 0x0c, NULL) < 0);
    cfg[0x51] = 0;
    assert(!pcie_capability_read_word(&p, 0x0a, &word) && word == 0);
    assert(pci_read_config_word(&p, 4095, &word) < 0);
    assert(pci_read_config_word(&p, 5, &word) < 0);
    assert(pci_read_config_dword(NULL, 0, &dword) < 0);
    unsigned writes = config_writes;
    assert(pci_write_config_word(&p, 5, 0) < 0);
    assert(pci_write_config_dword(&p, 4094, 0) < 0);
    assert(writes == config_writes);
    assert(pci_save_state(&p) < 0);
    assert(pci_load_saved_state(&p, NULL) < 0);
    p.vendor = 0x1002; p.device = 0x744c;
    wr32(0, 0x744c1002);
    assert(!pci_dev_is_disconnected(&p));
    wr32(0, UINT32_MAX);
    assert(pci_dev_is_disconnected(&p));
    wr32(0, 0x744c1002);
    cfg[0x51] = 0x80;
    cfg[0x80] = PCI_CAP_ID_EXP; cfg[0x81] = 0;
    wr16(0x82, 2);
    wr16(0x88, 0x123);
    wr16(0x90, 0x40);
    wr16(0xa8, 0x20);
    wr16(0xb0, 3);
    wr16(PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    wr32(0x10, 0x8000000c);
    assert(pci_save_state(&p) == 0);
    fail_state_alloc = 1;
    assert(!pci_store_saved_state(&p));
    fail_state_alloc = 0;
    struct pci_saved_state *saved = pci_store_saved_state(&p);
    assert(saved);
    fail_read_at = 0x18;
    assert(pci_save_state(&p) < 0); /* Failed capture keeps prior state intact. */
    fail_read_at = -1;
    wr16(PCI_COMMAND, 0);
    wr16(0x88, 0);
    wr16(0x90, 0);
    assert(!pci_load_saved_state(&p, saved));
    unsigned fault_before = faults;
    pci_restore_state(&p);
    assert(faults == fault_before && rd16(PCI_COMMAND) == 6);
    assert(rd16(0x88) == 0x123 && rd16(0x90) == 0x40);
    assert(rd32(0x10) == 0x8000000c);
    assert(!pci_load_saved_state(&p, saved));
    wr32(0x10, 0x9000000c);
    writes = config_writes;
    pci_restore_state(&p); /* A changed assignment is never repaired with BAR writes. */
    assert(faults == fault_before + 1 && config_writes == writes);
    wr32(0x10, 0x8000000c);
    assert(!pci_load_saved_state(&p, saved));
    wr16(0x88, 0);
    suppress_write_at = 0x88;
    pci_restore_state(&p);
    assert(faults == fault_before + 2);
    suppress_write_at = -1;
    assert(!pci_load_saved_state(&p, saved));
    pci_restore_state(&p);
    kfree(saved);
    wr16(0x8a, 0);
    assert(pci_wait_for_pending_transaction(&p) == 1);
    wr16(0x8a, 1 << 5);
    assert(pci_wait_for_pending_transaction(&p) == 0);
    wr16(0x8a, 0);

    /* Reference driver's read-only ReBAR v1 parser, including malformed tails. */
    wr32(0x140, 0x15 | (1u << 16));
    wr32(0x144, ((1u << 8) | (1u << 15)) << 4);
    wr32(0x148, (1u << 5) | (8u << 8));
    assert(pci_rebar_get_max_size(&p, 0) == 15);
    assert(pci_rebar_size_supported(&p, 0, 8));
    assert(!pci_rebar_size_supported(&p, 0, 9));
    assert(pci_rebar_size_to_bytes(15) == (32ULL << 30));
    assert(!pci_rebar_size_to_bytes(44));
    p.resource[0].start = 0x80000000;
    p.resource[0].end = p.resource[0].start + (256ULL << 20) - 1;
    writes = config_writes;
    assert(!pci_resize_resource(&p, 0, 8, 0));
    assert(pci_resize_resource(&p, 0, 15, 0) == -95);
    assert(config_writes == writes);
    wr32(0x148, (2u << 5) | (8u << 8));
    wr32(0x14c, (1u << 8) << 4);
    wr32(0x150, (8u << 8)); /* Duplicate BAR in second entry. */
    assert(pci_rebar_get_max_size(&p, 0) < 0);
    wr32(0x148, (1u << 5) | (8u << 8));
    cfg[0x51] = 0;
        assert(pci_alloc_irq_vectors(&p, 1, 1, PCI_IRQ_ALL_TYPES) < 0);
    armed_irq_vectors = 1;
    actual_irq_type = DEXT_PCI_IRQ_MSI;
    assert(pci_alloc_irq_vectors(&p, 1, 1, PCI_IRQ_MSIX) < 0);
    assert(pci_alloc_irq_vectors(&p, 1, 1, PCI_IRQ_MSI) == 1);
    assert(pci_irq_vector(&p, 0) == 0);
    assert(pci_irq_vector(&p, 1) < 0);
    armed_irq_vectors = 0;
    assert(pci_irq_vector(&p, 0) < 0);
    armed_irq_vectors = 1;
    assert(pci_free_irq_vectors(&p) == 0);
    actual_irq_type = DEXT_PCI_IRQ_MSIX;
    assert(pci_alloc_irq_vectors(&p, 1, 1, PCI_IRQ_MSI) < 0);
    assert(pci_alloc_irq_vectors(&p, 1, 1, PCI_IRQ_MSIX) == 1);
    assert(pci_free_irq_vectors(&p) == 0);
    assert(pci_reset_function(&p) < 0);
    /* Linux semantics: a mask the platform can serve is recorded as
     * requested; the bus limit and the narrower mask set the DMA width. */
    assert(!pci_dma_supported(&p, (1ULL << 32) - 1));
    assert(!pci_dma_supported(&p, (1ULL << 40) - 1));
    assert(pci_dma_supported(&p, (1ULL << 44) - 1));
    assert(pci_set_dma_mask(&p, DMA_BIT_MASK(32)) < 0);
    assert(dma_mask == DMA_BIT_MASK(44) && !dma_width);
    assert(pci_set_dma_mask(&p, DMA_BIT_MASK(64)) == 0);
    assert(dma_mask == DMA_BIT_MASK(64) && dma_width == 44);
    assert(pci_set_consistent_dma_mask(&p, DMA_BIT_MASK(32)) < 0);
    assert(p.dev.coherent_dma_mask == DMA_BIT_MASK(44));
    p.dev.bus_dma_limit = 0;
    assert(pci_set_consistent_dma_mask(&p, DMA_BIT_MASK(64)) == 0);
    assert(p.dev.coherent_dma_mask == DMA_BIT_MASK(64) && dma_width == 64);
    return 0;
}
