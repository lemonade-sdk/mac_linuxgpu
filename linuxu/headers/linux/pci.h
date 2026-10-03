/* linuxu: SHIM (third_party/linux/include/linux/pci.h)
 *
 * PCI API surface for the KMD. The IOPCIDevice backend is the dext's
 * job; this header provides only the struct + API surface
 * so unmodified upstream .c files compile. All non-trivial functions
 * are declared extern (runtime: linuxu/src/pci/pci.c).
 *
 * struct resource is defined here (self-contained subset).
 */
#ifndef LINUX_PCI_H
#define LINUX_PCI_H

#ifndef __iomem
#define __iomem
#endif

#include <linux/types.h>
#include <linux/pci_ids.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/atomic.h>

struct device;


/* resource flags (from upstream resource_ext.h subset) */
#define IORESOURCE_MEM		0x00000200UL
#define IORESOURCE_IO		0x00000100UL
#define IORESOURCE_BUSY		0x80000000UL
#define IORESOURCE_PREFETCH	0x00002000UL
#define IORESOURCE_MEM_64	0x00100000UL
#define IORESOURCE_SYSMEM	0x01000000UL
#define IORESOURCE_WINDOW	0x00200000UL
#define IORESOURCE_DISABLED	0x10000000UL

/* Linux indexes the expansion ROM after the six hardware BARs.  Keep the
 * bridge slots as well: callers may address any canonical resource index. */
enum {
	PCI_STD_RESOURCES,
	PCI_STD_RESOURCE_END = PCI_STD_RESOURCES + 6 - 1,
	PCI_ROM_RESOURCE,
#ifdef CONFIG_PCI_IOV
	PCI_IOV_RESOURCES,
	PCI_IOV_RESOURCE_END = PCI_IOV_RESOURCES + 6 - 1,
#endif
	PCI_BRIDGE_RESOURCES,
	PCI_BRIDGE_RESOURCE_END = PCI_BRIDGE_RESOURCES + 4 - 1,
	PCI_NUM_RESOURCES,
	DEVICE_COUNT_RESOURCE = PCI_NUM_RESOURCES,
};
#define PCI_CONFIG_SIZE		256

/* ---- PCI config space defines (from pci_regs.h subset) ---- */
#define PCI_COMMAND		0x04
#define PCI_COMMAND_IO		0x0001
#define PCI_COMMAND_MEMORY	0x0002
#define PCI_COMMAND_MASTER	0x0004
#define PCI_STATUS		0x06
#define PCI_STATUS_CAP_LIST	0x0010
#define PCI_CAPABILITY_LIST	0x34
#define PCI_CAP_LIST_ID		0
#define PCI_CAP_LIST_NEXT	1
#define PCI_CAP_ID_PM		0x01
#define PCI_PM_CTRL		0x04
#define PCI_PM_CTRL_STATE_MASK	0x0003

#define PCI_CLASS_DEVICE	0x0a
#define PCI_REVISION_ID		0x08
#define PCI_SUBSYSTEM_VEND_ID	0x2c
#define PCI_SUBSYSTEM_ID	0x2e

#define PCI_CAP_ID_MSI		0x05
#define PCI_CAP_ID_MSIX		0x11
#define PCI_MSIX_FLAGS		0x2

/* PCI bridge/PCIe capability registers (pci_regs.h subset) */
#define PCI_PRIMARY_BUS		0x18	/* Primary bus number */
#define PCI_CAP_ID_EXP		0x10	/* PCI Express */
#define PCI_EXP_DEVSTA		0x0a	/* Device Status */
#define  PCI_EXP_DEVSTA_CED	0x0001	/* Correctable Error Detected */
#define  PCI_EXP_DEVSTA_NFED	0x0002	/* Non-Fatal Error Detected */
#define  PCI_EXP_DEVSTA_FED	0x0004	/* Fatal Error Detected */
#define  PCI_EXP_DEVSTA_URD	0x0008	/* Unsupported Request Detected */
#define PCI_EXP_LNKSTA		0x12	/* Link Status */
#define PCI_EXT_CAP_ID		0x02
#define PCI_EXT_CAP_ID_ERR	0x01	/* Advanced Error Reporting */
#define PCI_ERR_UNCOR_STATUS	0x04	/* Uncorrectable Error Status */
#define PCI_ERR_COR_STATUS	0x10	/* Correctable Error Status */

#define PCI_D0	0
#define PCI_D1	1
#define PCI_D2	2
#define PCI_D3hot	3
#define PCI_D3cold	4

/* ---- struct pci_bus / msix_entry / vpd (subset) ---- */
struct pci_bus {
	unsigned int	number;
	unsigned int	secondary;
	unsigned int	subordinate;
	struct resource	*resource;
	struct device		*sysdata;
	void			*bridge;
	unsigned int	max_busnr;
	struct list_head	children;
	struct list_head	siblings;
	struct pci_bus		*parent;
};
struct pci_vpd { int dummy; };

/* ---- devfn slot/func encoding (upstream asm-generic/pci-bridge.h) ---- */
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn) ((devfn) >> 3)
#define PCI_FUNC(devfn) ((devfn) & 0x07)

struct msix_entry {
	union {
		u32 entry;
		struct {
			u32 address_lo;
			u32 address_hi;
			u16 data;
			u16 data_hi;
		};
	};
	unsigned int vector;
};

/* ---- struct resource (self-contained subset) ---- */
struct resource {
	struct device *owner;
	resource_size_t start;
	resource_size_t end;
	const char *name;
	unsigned long flags;
	struct resource *parent, *sibling, *child;
};

/* vendor 2026 ioport.h surface (kfd_migrate.c) */
static inline resource_size_t resource_size(struct resource *r)
{
	return r->end - r->start + 1;
}
/* vendor 2026 ioport.h globals (defined in linuxu/src/mm/dev_pagemap.c) */
extern struct resource iomem_resource;
extern struct resource ioport_resource;

extern struct resource *devm_request_free_mem_region(struct device *dev,
						     struct resource *parent,
						     resource_size_t size);
extern void devm_release_mem_region(struct device *dev,
				    resource_size_t start, resource_size_t size);
extern struct device_memory *devm_register_device_memory(struct device *dev);
extern void devm_unregister_device_memory(struct device *dev, struct device_memory *dm);


/* Only the matched endpoint is exposed by DriverKit. Preserve its assigned
 * BARs and ordinary PCIe controls; MSI routing remains owned by DriverKit. */
struct linuxu_pci_state {
	u32 header[16];
	u16 pcie_cap;
	u16 pcie_flags;
	u16 control[4];
};
struct pci_saved_state;

struct pci_dev {
	u16		vendor;
	u16		device;
	u16		subsystem_vendor;
	u16		subsystem_device;
	u32		class;
	u8		revision;
	u8		phfn;
	u16		msix_cap;
	int		msix_enabled;
	int		msi_enabled;
	unsigned int	no_msi:1;
	u32		romlen;
	phys_addr_t rom;
	unsigned int	rom_base;
	unsigned long	rom_attr;
	int		enable_cnt;
	unsigned int	transparent:1;
	int		is_pcie_device;
	unsigned int	multifunction:1;
	unsigned int	is_virtfn:1;
	unsigned int	is_physfn:1;
	unsigned int	pcie_capable:1;
	unsigned int	rom_attr_enabled:1;
	struct pci_bus	*bus;
	unsigned int	devfn;
	int		domain;
	int		irq;
	struct resource resource[DEVICE_COUNT_RESOURCE];
	unsigned long	irq_managed:1;
	void		*driver_data;
	void		*sysdata;
	struct device	dev;
	struct pci_vpd	vpd;
	int		is_bridge;
	int		cfg_size;
	unsigned int	pci_ats_enabled:1;
	unsigned int	ats_cap;
	u8		ats_stu;
	u32		saved_config_space[64];
	struct linuxu_pci_state saved_state;
	bool state_saved;
};

/* ---- static inline helpers ---- */
static inline struct pci_bus *pci_bus(struct pci_dev *dev)
{
	return dev->bus;
}

static inline int pci_domain_nr(struct pci_bus *bus)
{
	return 0;
}

static inline u16 pci_dev_id(struct pci_dev *dev)
{
	return (u16)((dev->bus->number << 8) | dev->devfn);
}

static inline int pci_dev_is_pcie(struct pci_dev *dev)
{
	return dev->is_pcie_device;
}

static inline int pci_is_pcie(struct pci_dev *dev)
{
	return dev->is_pcie_device;
}

static inline const char *pci_name(const struct pci_dev *dev)
{
	return "pci";
}

static inline resource_size_t pci_resource_start(struct pci_dev *dev, int bar)
{
	return dev->resource[bar].start;
}

static inline resource_size_t pci_resource_len(struct pci_dev *dev, int bar)
{
	if (!dev->resource[bar].flags)
		return 0;
	return dev->resource[bar].end - dev->resource[bar].start + 1;
}

static inline unsigned long pci_resource_flags(struct pci_dev *dev, int bar)
{
	return dev->resource[bar].flags;
}

static inline void *pci_get_drvdata(struct pci_dev *dev)
{
	return dev_get_drvdata(&dev->dev);
}

static inline void pci_set_drvdata(struct pci_dev *dev, void *data)
{
	dev_set_drvdata(&dev->dev, data);
}

extern void pci_set_master(struct pci_dev *dev);
extern void pci_clear_master(struct pci_dev *dev);

/* ---- API (runtime: linuxu/src/pci/pci.c) ---- */
extern int pci_enable_device(struct pci_dev *dev);
extern int pci_enable_device_mem(struct pci_dev *dev);
extern int pci_enable_device_busmaster(struct pci_dev *dev);
extern void pci_disable_device(struct pci_dev *dev);
extern int pci_dma_supported(struct pci_dev *dev, u64 mask);
extern int pci_set_dma_mask(struct pci_dev *dev, u64 mask);
extern int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask);
extern int pci_enable_bus_master(struct pci_dev *dev);
extern void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen);
extern void __iomem *pci_iomap_range(struct pci_dev *dev, int bar,
				     unsigned long offset, unsigned long maxlen);
extern void pci_iounmap(struct pci_dev *dev, void __iomem *base);
extern void __iomem *pci_mem_map(struct pci_dev *dev, int bar,
				 unsigned long offset, unsigned long maxlen);
extern void pci_mem_unmap(struct pci_dev *dev, void __iomem *base);
extern int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
extern int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);

/* PCIe capability access (vendor 2026 pci.h) */
extern int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
extern int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
extern int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
extern int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
extern int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
extern int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
extern int pci_save_state(struct pci_dev *dev);
extern void pci_restore_state(struct pci_dev *dev);
extern int pci_request_regions(struct pci_dev *dev, const char *res_name);
extern int pci_request_regions_exclusive(struct pci_dev *dev, const char *res_name);
extern void pci_release_regions(struct pci_dev *dev);
extern int pci_enable_msix(struct pci_dev *dev, struct msix_entry *entries, int nvec);
extern int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
				 int min_vecs, int max_vecs);
extern void pci_disable_msix(struct pci_dev *dev);
extern int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
				 unsigned int max_vecs, unsigned int flags);
extern int pci_alloc_irq_vectors_affinity(struct pci_dev *dev,
					  unsigned int min_vecs,
					  unsigned int max_vecs,
					  unsigned int flags);
extern int pci_free_irq_vectors(struct pci_dev *dev);
extern int pci_irq_vector(struct pci_dev *dev, unsigned int vec);
extern int pci_reset_bus(struct pci_dev *dev);
extern int pci_reset_function(struct pci_dev *dev);
extern int pci_reset_slot(struct pci_dev *dev);
extern void pci_set_power_state(struct pci_dev *dev, int state);
extern int pci_power_state(struct pci_dev *dev);
extern int pci_probe_reset_bus(struct pci_dev *dev);
extern int pci_slot_reset(struct pci_dev *dev);
extern int pci_wait_for_pending_transaction(struct pci_dev *dev);
extern struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev);

extern int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state);

extern int pci_find_ext_capability(struct pci_dev *dev, int cap);
extern int pci_find_capability(struct pci_dev *dev, int cap);
extern struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
						    unsigned int devfn);
extern struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *prev);
extern struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
				      struct pci_dev *prev);
extern void pci_dev_put(struct pci_dev *dev);
extern struct pci_dev *pci_dev_get(struct pci_dev *dev);
typedef enum {
	pci_channel_state_normal = 0,
	pci_channel_io_normal,
	pci_channel_io_frozen,
	pci_channel_io_disabled,
	pci_channel_io_perm_failure,
} pci_channel_state_t;

typedef enum {
	pci_ers_result_success = 0,
	pci_ers_result_fail,
	pci_ers_result_DISCONNECT,
	pci_ers_result_RECOVER,
	pci_ers_result_CAN_RECOVER,
} pci_ers_result_t;

struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev,
					   pci_channel_state_t error);
	pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*reset_prepare)(struct pci_dev *dev);
	void (*reset_done)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
};

struct pci_dynids { int dummy; };

/*
 * linuxu: SHIM — full definition when the complete device.h is in scope
 * (normal driver TUs); forward-declaration + pointer when only a partial
 * device.h fragment is visible (ioport.h re-enters pci.h before the
 * device.h include completes).
 */
#ifndef _LINUXU_PCI_DRIVER_DEFINED
#define _LINUXU_PCI_DRIVER_DEFINED
#include <linux/device.h>
struct pci_dynids;

/* vendor 2026 pci.h: struct device_driver is embedded in struct pci_driver
 * (not a pointer); the .driver.pm designator in amdgpu_drv.c depends on it. */
struct pci_driver {
	const char		*name;
	const struct pci_device_id *id_table;
	int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	int  (*suspend)(struct pci_dev *dev, int state);
	int  (*resume)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	int  (*sriov_configure)(struct pci_dev *dev, int num_vfs);
	const struct pci_error_handlers *err_handler;
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	struct device_driver driver;
	struct pci_dynids	dynids;
	bool driver_managed_dma;
};
#endif /* _LINUXU_PCI_DRIVER_DEFINED */

extern int pci_register_driver(struct pci_driver *drv);
extern void pci_unregister_driver(struct pci_driver *drv);
extern const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
						struct pci_dev *dev);
extern int pci_probe(struct pci_dev *dev);
extern void pci_remove(struct pci_dev *dev);
extern void pci_shutdown(struct pci_dev *dev);
extern int pci_resume(struct pci_dev *dev);
extern int pci_config_reset(struct pci_dev *dev);
extern struct pci_dev *pci_upstream_bridge(struct pci_dev *dev);
extern int pci_is_root_bus(struct pci_bus *bus);
extern int pci_device_is_present(struct pci_dev *dev);
extern int pci_dev_is_disconnected(struct pci_dev *dev);
static inline bool pci_is_enabled(const struct pci_dev *dev)
{
	return dev->enable_cnt > 0;
}
static inline u8 *pci_map_rom(struct pci_dev *dev, unsigned long *romlen) { (void)dev; if (romlen) *romlen = 0; return NULL; }
extern void pci_unmap_rom(struct pci_dev *dev, void __iomem *rom);
extern int pci_resize_resource(struct pci_dev *dev, int bar, int size,
			       int exclude_bars);
extern int pci_mmio_enabled(struct pci_dev *dev);
extern int pci_max_read_req_size(struct pci_dev *dev);
extern int pcie_get_mps(struct pci_dev *dev);
extern int pci_pcie_type(struct pci_dev *dev);
extern int pci_p2pdma_distance(struct pci_dev *orig,
			       struct pci_dev *peer, bool write);
extern int pci_rebar_get_max_size(struct pci_dev *dev, int bar);
extern int pci_rebar_bytes_to_size(u64 bytes);
extern resource_size_t pci_rebar_size_to_bytes(int size);
extern u64 pci_rebar_get_possible_sizes(struct pci_dev *dev, int bar);
extern bool pci_rebar_size_supported(struct pci_dev *dev, int bar, int size);
extern bool pcie_aspm_enabled(struct pci_dev *dev);
extern void pci_restore_msi_state(struct pci_dev *dev);
extern void pci_restore_config_space(struct pci_dev *dev);
extern int pci_error_detected(struct pci_dev *dev);
extern int pci_ignore_hotplug(struct pci_dev *dev);
extern int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u8 flags);
extern int pci_wake_from_d3(struct pci_dev *dev, u32 flags);
extern int pci_pr3_present(struct pci_dev *dev);
extern struct pci_dev *pci_devices(void);
#define pci_bus_for_each_resource(bus, res, i) \
	for ((i) = 0, (res) = (bus)->resource; (i) < PCI_BRIDGE_RESOURCES && (res); (i)++, (res) = NULL)


/* ---- error recovery / channel state (used by amdgpu.h ops) ---- */

#define PCI_ERROR_RESPONSE (~0ULL)
#define PCI_POSSIBLE_ERROR(x) ((x) == (typeof(x))PCI_ERROR_RESPONSE)


/* ---- MSI-X / IRQ capability flags (upstream linux/pci.h) ---- */
#define PCI_MSIX_FLAGS_ENABLE		0x8000
#define PCI_MSIX_FLAGS_PERVEC_ENABLE	0x4000
#define PCI_IRQ_LEGACY		(1 << 0)
#define PCI_IRQ_MSIX		(1 << 2)
#define PCI_IRQ_MSI		(1 << 1)
#define PCI_IRQ_PIN_SHARED	(1 << 3)
#define PCI_IRQ_INTX		(1 << 0)
#define PCI_IRQ_ALL_TYPES	(PCI_IRQ_LEGACY | PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_INTX)

static inline struct pci_dev *to_pci_dev(struct device *dev)
{
	return dev ? container_of(dev, struct pci_dev, dev) : NULL;
}
#endif /* LINUX_PCI_H */
#define IORESOURCE_UNSET	0x20000000
#define IORESOURCE_ROM_SHADOW	0x2
/* PCI vendor IDs (subset used by amdgpu_device.c) */
#ifndef PCI_VENDOR_ID_DELL
#define PCI_VENDOR_ID_DELL 0x1028
#endif
#ifndef PCI_EXT_CAP_ID_VNDR
#define PCI_EXT_CAP_ID_VNDR 0x0b
#endif
#ifndef PCI_VENDOR_ID_APPLE
#define PCI_VENDOR_ID_APPLE 0x106b
#endif
#ifndef PCI_VENDOR_ID_MICROSOFT
#define PCI_VENDOR_ID_MICROSOFT 0x1414
#endif
#ifndef PCI_VENDOR_ID_SAMSUNG
#define PCI_VENDOR_ID_SAMSUNG 0x144d
#endif
#ifndef PCI_VENDOR_ID_HYNIX
#define PCI_VENDOR_ID_HYNIX 0x1ae1
#endif
#ifndef PCI_VENDOR_ID_MICRON
#define PCI_VENDOR_ID_MICRON 0x1c58
#endif

/* IS_ENABLED comes from linux/list.h and the shared autoconf table. */
#define IS_ENABLED_CONFIG_PHYS_ADDR_T_64BIT 1

/* VGA resource types */
#ifndef VGA_RSRC_LEGACY_IO
#define VGA_RSRC_LEGACY_IO 0
#endif
#ifndef VGA_RSRC_LEGACY_MEM
#define VGA_RSRC_LEGACY_MEM 1
#endif
#ifndef VGA_RSRC_NORMAL_IO
#define VGA_RSRC_NORMAL_IO 2
#endif
#ifndef VGA_RSRC_NORMAL_MEM
#define VGA_RSRC_NORMAL_MEM 3
#endif

/* vga_switcheroo surface lives in <linux/vga_switcheroo.h> (vendor 2026) */
extern struct pci_dev *pcie_find_root_port(struct pci_dev *dev);

/* Kernel taint flags */
#ifndef TAINT_SOFTLOCKUP
#define TAINT_SOFTLOCKUP 18
#endif
#ifndef LOCKDEP_STILL_OK
#define LOCKDEP_STILL_OK 0
#endif
extern void add_taint(int taint, int lock);

#define PCI_EXP_DEVCAP2	0x24
#ifndef PCI_EXP_DEVCAP2_ATOMIC_COMP32
#define PCI_EXP_DEVCAP2_ATOMIC_COMP32 (1 << 4)
#endif
#ifndef PCI_EXP_DEVCAP2_ATOMIC_COMP64
#define PCI_EXP_DEVCAP2_ATOMIC_COMP64 (1 << 5)
#endif
#ifndef PCI_EXP_DEVCAP2_ATOMIC_REQ32
#define PCI_EXP_DEVCAP2_ATOMIC_REQ32 (1 << 0)
#endif
#ifndef PCI_EXP_DEVCAP2_ATOMIC_REQ64
#define PCI_EXP_DEVCAP2_ATOMIC_REQ64 (1 << 1)
#endif
#ifndef IS_ENABLED_CONFIG_PERF_EVENTS
#define IS_ENABLED_CONFIG_PERF_EVENTS 0
#endif

#ifndef SYSTEM_POWER_OFF
#define SYSTEM_POWER_OFF 3
#endif
#ifndef SYSTEM_POWER_UP
#define SYSTEM_POWER_UP 0
#endif
#ifndef SYSTEM_SUSPEND
#define SYSTEM_SUSPEND 2
#endif
#ifndef SYSTEM_REBOOT
#define SYSTEM_REBOOT 4

#endif

#ifndef IS_ENABLED_CONFIG_HOTPLUG_PCI_PCIE
#define IS_ENABLED_CONFIG_HOTPLUG_PCI_PCIE 0
#endif

/* PCI bus speeds */
#ifndef __LINUX_PCI_BUS_SPEED
#define __LINUX_PCI_BUS_SPEED
enum pci_bus_speed {
	PCI_SPEED_UNKNOWN = 0,
	PCI_SPEED_2_5GB = 1,
	PCI_SPEED_5_0GB = 2,
	PCI_SPEED_8_0GB = 3,
	PCI_SPEED_16_0GB = 4,
	PCI_SPEED_32_0GB = 5,
	PCI_SPEED_64_0GB = 6,
};
#endif

extern enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev);
extern const char *pci_speed_string(enum pci_bus_speed speed);
/* drivers/pci/pci-sysfs.c attribute groups of every PCI device. */
extern const struct attribute_group *pci_dev_groups[];

/* PCIe link widths */
#ifndef __LINUX_PCIE_LINK_WIDTH
#define __LINUX_PCIE_LINK_WIDTH
enum pcie_link_width {
	PCIE_LNK_WIDTH_UNKNOWN = 0,
	PCIE_LNK_WIDTH_X1 = 1,
	PCIE_LNK_WIDTH_X2 = 2,
	PCIE_LNK_WIDTH_X4 = 4,
	PCIE_LNK_WIDTH_X8 = 8,
	PCIE_LNK_WIDTH_X12 = 12,
	PCIE_LNK_WIDTH_X16 = 16,
	PCIE_LNK_WIDTH_X32 = 32,
};
#endif
extern enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev);
extern u32 pcie_bandwidth_available(struct pci_dev *dev,
		struct pci_dev **limiting_dev, enum pci_bus_speed *speed,
		enum pcie_link_width *width);

/* PCIe speeds */
#ifndef PCIE_SPEED_2_5GT
#define PCIE_SPEED_2_5GT 1
#endif
#ifndef PCIE_SPEED_5_0GT
#define PCIE_SPEED_5_0GT 2
#endif
#ifndef PCIE_SPEED_8_0GT
#define PCIE_SPEED_8_0GT 3
#endif
#ifndef PCIE_SPEED_16_0GT
#define PCIE_SPEED_16_0GT 4
#endif
#ifndef PCIE_SPEED_32_0GT
#define PCIE_SPEED_32_0GT 5
#endif
#ifndef PCIE_SPEED_64_0GT
#define PCIE_SPEED_64_0GT 6
#endif

#ifndef PCIE_LNK_X1
#define PCIE_LNK_X1 1
#endif
#ifndef PCIE_LNK_X2
#define PCIE_LNK_X2 2
#endif
#ifndef PCIE_LNK_X4
#define PCIE_LNK_X4 4
#endif
#ifndef PCIE_LNK_X8
#define PCIE_LNK_X8 8
#endif
#ifndef PCIE_LNK_X12
#define PCIE_LNK_X12 12
#endif
#ifndef PCIE_LNK_X16
#define PCIE_LNK_X16 16
#endif
#ifndef PCIE_LNK_X32
#define PCIE_LNK_X32 32
#endif

/* PCI AER results */
#ifndef PCI_ERS_RESULT_NEED_RESET
#define PCI_ERS_RESULT_NEED_RESET 4
#endif
#ifndef PCI_ERS_RESULT_RECOVERED
#define PCI_ERS_RESULT_RECOVERED 1
#endif
#ifndef PCI_ERS_RESULT_DISCONNECT
#define PCI_ERS_RESULT_DISCONNECT 3
#endif
#ifndef PCI_ERS_RESULT_NORMAL
#define PCI_ERS_RESULT_NORMAL 2
#define PCI_ERS_RESULT_CAN_RECOVER 3
#endif


/* Vendor ID register offset; PCI_VENDOR_ID_ATI is the AMD vendor value. */
#ifndef PCI_VENDOR_ID
#define PCI_VENDOR_ID 0x00
#endif

/* PCIe device types */
#ifndef PCI_EXP_TYPE_ENDPOINT
#define PCI_EXP_TYPE_ENDPOINT 0
#endif
#ifndef PCI_EXP_TYPE_LEGACY
#define PCI_EXP_TYPE_LEGACY 1
#endif
#ifndef PCI_EXP_TYPE_NORMAL
#define PCI_EXP_TYPE_NORMAL 2
#endif
#ifndef PCI_EXP_TYPE_UPSTREAM
#define PCI_EXP_TYPE_UPSTREAM 5
#endif
#ifndef PCI_EXP_TYPE_DOWNSTREAM
#define PCI_EXP_TYPE_DOWNSTREAM 6
#endif
#ifndef PCI_EXP_TYPE_ROOT_PORT
#define PCI_EXP_TYPE_ROOT_PORT 4
#endif
#ifndef PCI_EXP_TYPE_PCI_BRIDGE
#define PCI_EXP_TYPE_PCI_BRIDGE 7
#endif
#ifndef PCI_EXP_TYPE_PCIE_BRIDGE
#define PCI_EXP_TYPE_PCIE_BRIDGE 8
#endif
#ifndef PCI_EXP_TYPE_PCI_SLT
#define PCI_EXP_TYPE_PCI_SLT 8
#endif
#ifndef PCI_EXP_TYPE_PCIE_SLT
#define PCI_EXP_TYPE_PCIE_SLT 9
#endif
#ifndef PCI_EXP_TYPE_PCI_SWITCH
#define PCI_EXP_TYPE_PCI_SWITCH 10
#endif
#ifndef PCI_EXP_TYPE_PCIE_SWITCH
#define PCI_EXP_TYPE_PCIE_SWITCH 11


#endif
