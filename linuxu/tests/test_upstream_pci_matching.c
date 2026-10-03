/* The runner extracts the unmodified AMDGPU table from its current source.
 * Exercise the production matcher with GPUs from several families and with
 * the other PCI functions an AMD graphics card exposes. No PCI access,
 * driver registration, or probe occurs. */
#include <assert.h>
#include <stdio.h>

#if TEST_INCLUDE_ORDER == 0
#include <linux/mod_devicetable.h>
#include <linux/pci.h>
#elif TEST_INCLUDE_ORDER == 1
#include <linux/pci.h>
#include <linux/mod_devicetable.h>
#else
#include <linux/device.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#endif
#include <drm/amd_asic_type.h>
#include "upstream_pci_table.inc"

_Static_assert(PCI_CLASS_DISPLAY_VGA == 0x0300, "VGA class/subclass");
_Static_assert(PCI_CLASS_DISPLAY_3D == 0x0302, "3D class/subclass");
_Static_assert(PCI_CLASS_DISPLAY_OTHER == 0x0380, "other display class/subclass");
_Static_assert(PCI_CLASS_ACCELERATOR_PROCESSING == 0x1200, "accelerator class/subclass");

static const struct pci_device_id *match(unsigned short vendor,
                                         unsigned short device,
                                         unsigned int class_code)
{
    struct pci_dev dev = {
        .vendor = vendor, .device = device, .revision = 0xc1,
        .subsystem_vendor = 0x1002, .subsystem_device = 0x0b36,
        .class = class_code,
    };
    return pci_match_id(pciidlist, &dev);
}

/* A discovery-era function is matched by the class entries alone. */
static void check_class(unsigned short device, unsigned int class_code,
                        int should_match)
{
    const struct pci_device_id *id = match(0x1002, device, class_code);
    assert(!!id == should_match);
    if (id) {
        assert(id->driver_data == CHIP_IP_DISCOVERY);
        assert(id->class == class_code && id->class_mask == 0xffffff);
    }
}

/* A pre-discovery family is matched by its explicit table entry, for any
 * class, and keeps the upstream ASIC type. */
static void check_explicit(unsigned short device, unsigned long chip)
{
    const struct pci_device_id *id = match(0x1002, device, 0x030000);
    assert(id && (id->driver_data & AMD_ASIC_MASK) == chip);
    assert(id->device == device);
}

int main(void)
{
    /* Explicit upstream entries from different generations. */
    check_explicit(0x6798, CHIP_TAHITI);          /* GFX6 / SI */
    check_explicit(0x67DF, CHIP_POLARIS10);       /* GFX8 */
    check_explicit(0x687F, CHIP_VEGA10);          /* GFX9 */
    check_explicit(0x66AF, CHIP_VEGA20);          /* GFX9 */
    check_explicit(0x731F, CHIP_NAVI10);          /* GFX10.1 */
    check_explicit(0x73BF, CHIP_SIENNA_CICHLID);  /* GFX10.3 */

    /* IP-discovery parts (GFX11, GFX12, APUs, accelerators, and IDs newer
     * than any list) are matched by class, with the IPs read from the
     * on-die discovery table at probe time. */
    const unsigned short discovery[] = { 0x744C, 0x7480, 0x7550, 0x7590, 0x15BF, 0x0001 };
    for (unsigned i = 0; i < sizeof(discovery) / sizeof(discovery[0]); i++) {
        check_class(discovery[i], 0x030000, 1);
        check_class(discovery[i], 0x038000, 1);
        check_class(discovery[i], 0x120000, 1);
        /* The pinned upstream table has no generic DISPLAY_3D entry, and
         * the class entries require programming interface 0. */
        check_class(discovery[i], 0x030200, 0);
        check_class(discovery[i], 0x030001, 0);
    }

    /* The other functions of AMD graphics cards are never amdgpu devices:
     * HDMI/DP audio, the card's PCIe switch ports, USB controllers. */
    assert(!match(0x1002, 0xAAA0, 0x040300));   /* SI-era HDMI audio */
    assert(!match(0x1002, 0xAB38, 0x040300));   /* Navi 1x HDMI audio */
    assert(!match(0x1002, 0xAB30, 0x040300));   /* Navi 3x HDMI audio */
    assert(!match(0x1002, 0x1478, 0x060400));   /* Navi switch upstream port */
    assert(!match(0x1002, 0x1479, 0x060400));   /* Navi switch downstream port */
    assert(!match(0x1002, 0x7444, 0x0c0330));   /* USB xHCI on a GPU board */
    assert(!match(0x1002, 0x7550, 0x040300));   /* a GPU ID on an audio class */
    assert(!match(0x1002, 0x7550, 0x020000));

    /* Other vendors never match, whatever their class. */
    assert(!match(0x10de, 0x2684, 0x030000));
    assert(!match(0x8086, 0x56a0, 0x030000));
    assert(!match(0x1022, 0x1480, 0x060000));
    puts("upstream PCI matching: passed");
    return 0;
}
