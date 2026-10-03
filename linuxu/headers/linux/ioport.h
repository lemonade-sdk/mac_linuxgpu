/* linuxu: SHIM (third_party/linux/include/linux/ioport.h) */
#ifndef __LINUX_IOPORT_H
#define __LINUX_IOPORT_H
#include <linux/types.h>
#include <linux/pci.h>

struct resource;
/* struct resource is completed by <linux/pci.h> */
extern struct resource iomem_resource;
extern struct resource ioport_resource;

/* resource_size / devm_request_free_mem_region / devm_release_mem_region /
 * devm_register_device_memory live in <linux/pci.h> where struct resource is
 * fully defined. */

#endif
