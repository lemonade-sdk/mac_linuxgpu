/* PCI class/subclass values from third_party/linux/include/linux/pci_ids.h.
 * These are 16-bit values; a pci_device_id class field shifts them by 8
 * to make room for the programming-interface byte. */
#ifndef LINUXU_PCI_IDS_H
#define LINUXU_PCI_IDS_H

#define PCI_CLASS_DISPLAY_VGA             0x0300
#define PCI_CLASS_DISPLAY_3D              0x0302
#define PCI_CLASS_DISPLAY_OTHER           0x0380
#define PCI_CLASS_ACCELERATOR_PROCESSING  0x1200

#endif
