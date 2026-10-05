/* linuxu shim: PCI resource and config-space access.  The DriverKit build
 * uses the matched IOPCIDevice; the host build keeps fake resources for tests. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/delay.h>

struct bus_type pci_bus_type = { .name = "pci" };

/* ---- rt-side hooks (implemented in amdgpu-rt/device.c; the
 * canonical prototypes live in <rt/rt.h>, declared with
 * rt_irq_handler_fn == irqreturn_t(*)(int, void*) — the same type as
 * irq_handler_t below, so request_irq/free_irq forward straight
 * through). */
#include <rt/rt.h>
#include <rt/ttm_cleanup.h>
#ifdef LINUXU_DEXT_DK
#include <rt/dext_pci.h>
#endif

/* ---- enable/disable ---- */
/*
 * T1 host backend: BAR setup.  The DriverKit backend fills the resources
 * from the matched device's actual BARs (dext_pci_snapshot); nothing here
 * applies to a real device.  Host tests have no device, so they get a
 * generic fake topology (a large VRAM aperture in BAR0 and a register BAR
 * in BAR5) whose tokens are backed by host shadows.
 */
#define LINUXU_FAKE_BAR0_SIZE  (16ULL * 1024 * 1024 * 1024) /* 16 GB VRAM */
#define LINUXU_FAKE_BAR5_SIZE  (512 * 1024)
#define LINUXU_FAKE_BAR0_BASE  0x40000000000ULL
#define LINUXU_FAKE_BAR5_BASE  0x40010000000ULL

#ifdef LINUXU_DEXT_DK
/* The Linux lookup API reports zero for both an absent capability and a
 * failed lookup. Power transitions must distinguish those cases before
 * enabling PCI memory access, so validate the complete conventional list. */
static int pci_dk_find_capability(struct pci_dev *dev, int cap, int *found)
{
	u16 status;
	u8 pos, id, next;
	unsigned char seen[256] = {0};
	int r, match = 0;

	if (!dev || !found || cap <= 0 || cap > UINT8_MAX)
		return -EINVAL;
	*found = 0;
	r = pci_read_config_word(dev, PCI_STATUS, &status);
	if (r) return r;
	if (status == UINT16_MAX) return -ENODEV;
	if (!(status & PCI_STATUS_CAP_LIST)) return 0;
	r = pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos);
	if (r) return r;
	while (pos) {
		if (pos < 0x40 || pos > 0xfc || (pos & 3) || seen[pos])
			return -EIO;
		seen[pos] = 1;
		r = pci_read_config_byte(dev, pos + PCI_CAP_LIST_ID, &id);
		if (r) return r;
		if (id == UINT8_MAX) return -ENODEV;
		r = pci_read_config_byte(dev, pos + PCI_CAP_LIST_NEXT, &next);
		if (r) return r;
		if (id == (u8)cap && !match) match = pos;
		pos = next;
	}
	*found = match;
	return 0;
}

static int pci_dk_set_command(struct pci_dev *dev, u16 bits, int enable)
{
	u16 before, after;
	int r;

	if (!dev)
		return -22;
	r = pci_read_config_word(dev, PCI_COMMAND, &before);
	if (r)
		return r;
	if (before == UINT16_MAX) return -ENODEV;
	after = enable ? (before | bits) : (before & ~bits);
	if (after != before) {
		r = pci_write_config_word(dev, PCI_COMMAND, after);
		if (r)
			return r;
	}
	r = pci_read_config_word(dev, PCI_COMMAND, &after);
	if (r)
		return r;
	if (after == UINT16_MAX) return -ENODEV;
	return ((after & bits) == (enable ? bits : 0)) ? 0 : -5;
}

static int pci_dk_force_d0(struct pci_dev *dev)
{
	u16 pmcsr;
	int cap, r;

	if (!dev)
		return -22;
	r = pci_dk_find_capability(dev, PCI_CAP_ID_PM, &cap);
	if (r)
		return r;
	if (!cap)
		return 0; /* no PM capability: no software power-state transition */
	if (cap + PCI_PM_CTRL + sizeof(pmcsr) > 256) return -EIO;
	r = pci_read_config_word(dev, cap + PCI_PM_CTRL, &pmcsr);
	if (r)
		return r;
	if (pmcsr == UINT16_MAX) return -ENODEV;
	if ((pmcsr & PCI_PM_CTRL_STATE_MASK) == PCI_D0)
		return 0;
	r = pci_write_config_word(dev, cap + PCI_PM_CTRL,
				  pmcsr & ~PCI_PM_CTRL_STATE_MASK);
	if (r)
		return r;
	/* PCI PM spec: D3hot -> D0 needs a 10 ms recovery time before the
	 * function may be accessed (Linux PCI_PM_D3HOT_WAIT). */
	msleep(10);
	r = pci_read_config_word(dev, cap + PCI_PM_CTRL, &pmcsr);
	if (!r && pmcsr == UINT16_MAX) return -ENODEV;
	return r ? r : ((pmcsr & PCI_PM_CTRL_STATE_MASK) == PCI_D0 ? 0 : -5);
}
#endif

void linuxu_pci_setup_bar(struct pci_dev *dev, int bar,
			   phys_addr_t base, resource_size_t size)
{
	if (bar < 0 || bar >= DEVICE_COUNT_RESOURCE)
		return;
	dev->resource[bar].start = (resource_size_t)base;
	dev->resource[bar].end = (resource_size_t)base + size - 1;
	dev->resource[bar].name = "linuxu-fake-bar";
	dev->resource[bar].flags = IORESOURCE_MEM | IORESOURCE_BUSY;
}

void linuxu_pci_default_bars(struct pci_dev *dev)
{
#ifndef LINUXU_DEXT_DK
	linuxu_pci_setup_bar(dev, 0, LINUXU_FAKE_BAR0_BASE, LINUXU_FAKE_BAR0_SIZE);
	linuxu_pci_setup_bar(dev, 5, LINUXU_FAKE_BAR5_BASE, LINUXU_FAKE_BAR5_SIZE);
#else
	(void)dev;
#endif
}

int pci_resource_start_is_valid(struct pci_dev *dev, int bar)
{
#ifdef LINUXU_DEXT_DK
	return dev && bar >= 0 && bar < 6 &&
		dev->resource[bar].flags && dev->resource[bar].start;
#else
	(void)dev; (void)bar;
	return 0;
#endif
}

int pci_enable_device(struct pci_dev *dev)
{
	if (!dev)
		return -22;
#ifdef LINUXU_DEXT_DK
	/* Which BARs a function needs is the driver's business (amdgpu maps
	 * BAR5 or BAR2 for registers depending on the ASIC family).  Enabling
	 * only requires that the snapshot published some memory BAR. */
	int any_memory_bar = 0;
	for (int bar = 0; bar < 6; bar++) /* standard BARs */
		if (dev->resource[bar].flags & IORESOURCE_MEM)
			any_memory_bar = 1;
	if (!any_memory_bar)
		return -19;
	if (!dev->enable_cnt) {
		int r = pci_dk_force_d0(dev);
		if (r)
			return r;
		r = pci_dk_set_command(dev, PCI_COMMAND_MEMORY, 1);
		if (r)
			return r;
	}
#else
		/* first enable: publish the BARs (host default topology) */
		if (!dev->enable_cnt) {
			/* leave a preconfigured resource[0] untouched; only
			 * fill bars the dext/host has not set up yet */
			if (!dev->resource[0].start &&
			    !dev->resource[0].end)
				linuxu_pci_setup_bar(dev, 0, LINUXU_FAKE_BAR0_BASE,
						     LINUXU_FAKE_BAR0_SIZE);
			if (!dev->resource[5].start &&
			    !dev->resource[5].end)
				linuxu_pci_setup_bar(dev, 5, LINUXU_FAKE_BAR5_BASE,
						     LINUXU_FAKE_BAR5_SIZE);
		}
#endif
	dev->enable_cnt++;
	return 0;
}

int pci_enable_device_mem(struct pci_dev *dev)
{
	return pci_enable_device(dev);
}

int pci_enable_device_busmaster(struct pci_dev *dev)
{
	int r = pci_enable_device(dev);
	if (r)
		return r;
	r = pci_enable_bus_master(dev);
	if (r)
		pci_disable_device(dev);
	return r;
}

void pci_disable_device(struct pci_dev *dev)
{
	if (dev && dev->enable_cnt) {
		dev->enable_cnt--;
#ifdef LINUXU_DEXT_DK
		if (!dev->enable_cnt &&
		    pci_dk_set_command(dev, PCI_COMMAND_MEMORY, 0))
			dev_warn(&dev->dev, "could not clear PCI MEM enable\n");
#endif
	}
}

int pci_dma_supported(struct pci_dev *dev, u64 mask)
{
	return dev && dma_supported(&dev->dev, mask, NULL);
}

int pci_set_dma_mask(struct pci_dev *dev, u64 mask)
{
	return dev ? dma_set_mask(&dev->dev, mask) : -EINVAL;
}

int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask)
{
	return dev ? dma_set_coherent_mask(&dev->dev, mask) : -EINVAL;
}

int pci_enable_bus_master(struct pci_dev *dev)
{
#ifdef LINUXU_DEXT_DK
	return pci_dk_set_command(dev, PCI_COMMAND_MASTER, 1);
#else
	(void)dev;
	return 0; /* host backend: DMA is in-process, bus master is a no-op */
#endif
}

void pci_set_master(struct pci_dev *dev)
{
#ifdef LINUXU_DEXT_DK
	if (pci_enable_bus_master(dev) && dev)
		dev_warn(&dev->dev, "could not enable PCI bus mastering\n");
#else
	(void)dev; /* host backend: in-process DMA, no command-word update */
#endif
}

void pci_clear_master(struct pci_dev *dev)
{
#ifdef LINUXU_DEXT_DK
	if (pci_dk_set_command(dev, PCI_COMMAND_MASTER, 0) && dev)
		dev_warn(&dev->dev, "could not disable PCI bus mastering\n");
#else
	(void)dev;
#endif
}

/* ---- iomap → fake-MMIO token ---- */
extern void *rt_mmio_iomap(struct pci_dev *dev, int bar,
			   unsigned long maxlen);
extern void rt_mmio_iounmap(struct pci_dev *dev, void *base);

void __iomem *pci_iomap(struct pci_dev *dev, int bar,
			unsigned long maxlen)
{
	return rt_mmio_iomap(dev, bar, maxlen);
}

void __iomem *pci_iomap_range(struct pci_dev *dev, int bar,
			      unsigned long offset,
			      unsigned long maxlen)
{
#ifdef LINUXU_DEXT_DK
	if (!dev || bar < 0 || bar >= 6 ||
	    offset >= RT_MMIO_DK_TOKEN_STRIDE ||
	    offset >= pci_resource_len(dev, bar) ||
	    (maxlen && (maxlen > RT_MMIO_DK_TOKEN_STRIDE - offset ||
	                maxlen > pci_resource_len(dev, bar) - offset)))
		return NULL;
	void *base = pci_iomap(dev, bar, 0);
	return base ? (void __iomem *)((uintptr_t)base + offset) : NULL;
#else
	(void)offset;
	return pci_iomap(dev, bar, maxlen);
#endif
}

void pci_iounmap(struct pci_dev *dev, void __iomem *base)
{
	rt_mmio_iounmap(dev, (void *)base);
}

void __iomem *pci_mem_map(struct pci_dev *dev, int bar,
			  unsigned long offset, unsigned long maxlen)
{
#ifdef LINUXU_DEXT_DK
	return pci_iomap_range(dev, bar, offset, maxlen);
#else
	(void)offset;
	return pci_iomap(dev, bar, maxlen);
#endif
}

void pci_mem_unmap(struct pci_dev *dev, void __iomem *base)
{
	pci_iounmap(dev, (void *)base);
}

/* ---- config space (stub table on the dev) ---- */
/* The upstream partner port (pci.h). One per process, like the endpoint
 * every configuration access in this file reaches. */
static struct pci_dev linuxu_partner;
static u16 linuxu_partner_pcie_caps;
static u32 linuxu_partner_link_caps;
static int linuxu_partner_known;

static int pci_is_partner(const struct pci_dev *dev)
{
	return dev == &linuxu_partner;
}

static int pci_config_access_valid(const struct pci_dev *dev, int where,
				   unsigned width)
{
	unsigned limit;
	/* Every access below reaches the GPU endpoint whatever dev says; the
	 * partner stand-in has no configuration space of its own. */
	if (!dev || pci_is_partner(dev) || where < 0 || ((unsigned)where & (width - 1)))
		return 0;
	limit = dev->cfg_size > 0 ? (unsigned)dev->cfg_size : 4096;
	if (limit > 4096) limit = 4096;
	return width <= limit && (unsigned)where <= limit - width;
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	if (!val) return -EINVAL;
	*val = UINT8_MAX;
	if (!pci_config_access_valid(dev, where, 1)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_read8((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where;
	*val = 0;
	return 0; /* TODO(linuxu): ConfigurationRead8 */
#endif
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	if (!val) return -EINVAL;
	*val = UINT16_MAX;
	if (!pci_config_access_valid(dev, where, 2)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_read16((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where;
	*val = 0;
	return 0;
#endif
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
	if (!val) return -EINVAL;
	*val = UINT32_MAX;
	if (!pci_config_access_valid(dev, where, 4)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_read32((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where;
	*val = 0;
	return 0;
#endif
}

int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	if (!pci_config_access_valid(dev, where, 1)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_write8((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where; (void)val;
	return 0;
#endif
}

int pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	if (!pci_config_access_valid(dev, where, 2)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_write16((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where; (void)val;
	return 0;
#endif
}

int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
	if (!pci_config_access_valid(dev, where, 4)) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	return dext_pci_config_write32((uint64_t)where, val) ? -19 : 0;
#else
	(void)dev; (void)where; (void)val;
	return 0;
#endif
}

/* PCIe register positions are relative to the capability, not config zero.
 * Version-one devices have no Device/Link/Slot control-two registers. */
static int pcie_capability_offset(struct pci_dev *dev, int pos, unsigned width,
				  int *offset)
{
	int cap = 0;
	if (!dev || pos < 0 || ((unsigned)pos & (width - 1)) ||
	    (unsigned)pos > 0x3c - width)
		return -EINVAL;
	*offset = 0;
#ifdef LINUXU_DEXT_DK
	int r = pci_dk_find_capability(dev, PCI_CAP_ID_EXP, &cap);
	if (r) return r;
#else
	cap = pci_find_capability(dev, PCI_CAP_ID_EXP);
#endif
	if (!cap) return 0;
	if ((unsigned)cap + (unsigned)pos + width > PCI_CONFIG_SIZE)
		return -EIO;
	if (pos >= 0x24) {
		u16 flags;
		int r = pci_read_config_word(dev, cap + 2, &flags);
		if (r) return r;
		if (flags == UINT16_MAX) return -ENODEV;
		if ((flags & 15) < 2) return 0;
	}
	*offset = cap + pos;
	return 0;
}

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	int offset, r;
	if (!val) return -EINVAL;
	*val = 0;
	if (pci_is_partner(dev)) {
		if (pos != 2) return -EINVAL;
		*val = linuxu_partner_pcie_caps;
		return 0;
	}
	r = pcie_capability_offset(dev, pos, sizeof(*val), &offset);
	return r || !offset ? r : pci_read_config_word(dev, offset, val);
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	int offset, r;
	if (!val) return -EINVAL;
	*val = 0;
	if (pci_is_partner(dev)) {
		if (pos != 0x0c) return -EINVAL;
		*val = linuxu_partner_link_caps;
		return 0;
	}
	r = pcie_capability_offset(dev, pos, sizeof(*val), &offset);
	return r || !offset ? r : pci_read_config_dword(dev, offset, val);
}

/* Configuration snapshots are published atomically. A partial read must
 * never replace a prior usable snapshot. Callers hold their device reference. */
static pthread_mutex_t pci_state_lock = PTHREAD_MUTEX_INITIALIZER;
struct pci_saved_state { struct linuxu_pci_state state; };
static const int pci_control_offsets[4] = { 0x08, 0x10, 0x28, 0x30 };

static int pci_capture_state(struct pci_dev *dev, struct linuxu_pci_state *state)
{
	int r, cap;
	if (!dev || !state) return -EINVAL;
	memset(state, 0, sizeof(*state));
	for (unsigned i = 0; i < 16; ++i) {
		r = pci_read_config_dword(dev, i * 4, &state->header[i]);
		if (r) return r;
	}
	if (!dev->vendor || dev->vendor == UINT16_MAX ||
	    state->header[0] != ((u32)dev->device << 16 | dev->vendor) ||
	    (u16)state->header[1] == UINT16_MAX ||
	    (state->header[3] & 0x007f0000u)) return -ENODEV;
#ifdef LINUXU_DEXT_DK
	r = pci_dk_find_capability(dev, PCI_CAP_ID_EXP, &cap);
	if (r) return r;
#else
	cap = pci_find_capability(dev, PCI_CAP_ID_EXP);
#endif
	state->pcie_cap = cap;
	if (!cap) return 0;
	r = pci_read_config_word(dev, cap + 2, &state->pcie_flags);
	if (r || state->pcie_flags == UINT16_MAX) return r ? r : -ENODEV;
	/* The provider is an endpoint. Bridge/link ownership is not exported. */
	unsigned type = (state->pcie_flags >> 4) & 15;
	if (type != PCI_EXP_TYPE_ENDPOINT && type != PCI_EXP_TYPE_LEGACY)
		return -EOPNOTSUPP;
	unsigned n = (state->pcie_flags & 15) > 1 ? 4 : 2;
	for (unsigned i = 0; i < n; ++i) {
		if (cap + pci_control_offsets[i] + 2 > 256) return -EIO;
		r = pci_read_config_word(dev, cap + pci_control_offsets[i], &state->control[i]);
		if (r || state->control[i] == UINT16_MAX) return r ? r : -ENODEV;
	}
	/* Do not cache a transient FLR/retrain request for later replay. */
	if ((state->control[0] & 0x8000) || (state->control[1] & 0x20))
		return -EBUSY;
	return 0;
}

int pci_save_state(struct pci_dev *dev)
{
	struct linuxu_pci_state state;
	pthread_mutex_lock(&pci_state_lock);
	int r = pci_capture_state(dev, &state);
	if (!r) {
		dev->saved_state = state;
		memcpy(dev->saved_config_space, state.header, sizeof(state.header));
		dev->state_saved = true;
	}
	pthread_mutex_unlock(&pci_state_lock);
	return r;
}

static int pci_restore_state_locked(struct pci_dev *dev,
				     const struct linuxu_pci_state *saved)
{
	struct linuxu_pci_state observed;
	int r = pci_capture_state(dev, &observed);
	if (r) return r;
	/* DriverKit Reset restores the host's PCI assignment itself. Refuse a
	 * changed aperture instead of writing BARs behind the host bridge. */
	if (observed.header[0] != saved->header[0] ||
	    observed.pcie_cap != saved->pcie_cap ||
	    observed.pcie_flags != saved->pcie_flags ||
	    memcmp(observed.header + 4, saved->header + 4, 6 * sizeof(u32)))
		return -ESTALE;
#ifdef LINUXU_DEXT_DK
	r = pci_dk_force_d0(dev);
	if (r) return r;
#endif
	unsigned n = !saved->pcie_cap ? 0 : (saved->pcie_flags & 15) > 1 ? 4 : 2;
	for (unsigned i = 0; i < n; ++i) {
		if (observed.control[i] == saved->control[i]) continue;
		int where = saved->pcie_cap + pci_control_offsets[i];
		u16 after;
		r = pci_write_config_word(dev, where, saved->control[i]);
		if (!r) r = pci_read_config_word(dev, where, &after);
		if (r || after != saved->control[i]) return r ? r : -EIO;
	}
	/* Write only the command half; the adjacent status is write-one-clear.
	 * Command is restored last, after aperture/control validation. */
	u16 command = (u16)saved->header[1], after;
	if ((u16)observed.header[1] != command) {
		r = pci_write_config_word(dev, PCI_COMMAND, command);
		if (r) return r;
	}
	r = pci_read_config_word(dev, PCI_COMMAND, &after);
	return r ? r : after == command ? 0 : -EIO;
}

void pci_restore_state(struct pci_dev *dev)
{
	if (!dev) return;
	pthread_mutex_lock(&pci_state_lock);
	int r = dev->state_saved ? pci_restore_state_locked(dev, &dev->saved_state) : 0;
	if (!r) dev->state_saved = false;
	pthread_mutex_unlock(&pci_state_lock);
	if (r) {
#ifdef LINUXU_DEXT_DK
		/* This Linux API has no return value; prevent further runtime
		 * submission through the established transport-failure latch. */
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_CONFIG, PCI_COMMAND);
#endif
		dev_warn(&dev->dev, "PCI state restore failed: %d\n", r);
	}
}

int pci_request_regions(struct pci_dev *dev, const char *res_name)
{
	(void)res_name;
	return 0;
}

int pci_request_regions_exclusive(struct pci_dev *dev,
				  const char *res_name)
{
	return pci_request_regions(dev, res_name);
}

void pci_release_regions(struct pci_dev *dev)
{
	(void)dev;
}

/* ---- MSI-X ---- */
int pci_enable_msix(struct pci_dev *dev, struct msix_entry *entries,
		    int nvec)
{
#ifdef LINUXU_DEXT_DK
	int r;
	if (nvec != 1)
		return -95;
	r = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSIX);
	if (r < 0)
		return r;
	if (entries)
		entries[0].vector = 0;
	return 0;
#else
	(void)entries;
	dev->msix_enabled = nvec;
	return 0; /* TODO(linuxu): MSI-X setup via IOInterruptDispatchSource */
#endif
}

int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
			  int min_vecs, int max_vecs)
{
#ifdef LINUXU_DEXT_DK
	if (min_vecs > 1 || max_vecs < 1)
		return -95;
	return pci_enable_msix(dev, entries, 1) ? -95 : 1;
#else
	int n;

	for (n = min_vecs; n <= max_vecs; n++)
		if (pci_enable_msix(dev, entries, n) == 0)
			return n;
	return -28; /* -ENOSPC */
#endif
}

void pci_disable_msix(struct pci_dev *dev)
{
	dev->msix_enabled = 0;
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
			  unsigned int max_vecs, unsigned int flags)
{
#ifdef LINUXU_DEXT_DK
	unsigned int armed = 0, type = DEXT_PCI_IRQ_NONE;
	if (!dev || min_vecs > 1 || max_vecs < 1 ||
	    dext_pci_irq_status(&armed, &type) || armed < 1)
		return -95;
	if (type == DEXT_PCI_IRQ_MSIX && (flags & PCI_IRQ_MSIX)) {
		dev->msix_enabled = 1;
		dev->msi_enabled = 0;
	} else if (type == DEXT_PCI_IRQ_MSI && (flags & PCI_IRQ_MSI)) {
		dev->msi_enabled = 1;
		dev->msix_enabled = 0;
	} else {
		return -95;
	}
	return 1; /* reuse the IOService-owned, armed vector zero */
#else
	(void)flags;
	return pci_enable_msix_range(dev, NULL, min_vecs, max_vecs);
#endif
}

int pci_alloc_irq_vectors_affinity(struct pci_dev *dev,
				   unsigned int min_vecs,
				   unsigned int max_vecs,
				   unsigned int flags)
{
#ifdef LINUXU_DEXT_DK
	return pci_alloc_irq_vectors(dev, min_vecs, max_vecs, flags);
#else
	(void)dev; (void)min_vecs; (void)max_vecs; (void)flags;
	return 0;
#endif
}

static int __unused_pci_alloc_irq_vectors_affinity(struct pci_dev *dev,
			   struct msix_entry *entries,
			   unsigned int min_vecs,
			   unsigned int max_vecs,
			   unsigned int flags)
{
	(void)entries;
	(void)flags;
	return pci_alloc_irq_vectors(dev, min_vecs, max_vecs, 0);
}

int pci_free_irq_vectors(struct pci_dev *dev)
{
	if (!dev)
		return -22;
	pci_disable_msix(dev);
	dev->msi_enabled = 0;
	return 0;
}

int pci_irq_vector(struct pci_dev *dev, unsigned int vec)
{
#ifdef LINUXU_DEXT_DK
	unsigned int armed = 0, type = DEXT_PCI_IRQ_NONE;
	if (!dev || (!dev->msix_enabled && !dev->msi_enabled) || vec != 0)
		return -22;
	if (dext_pci_irq_status(&armed, &type) || armed < 1 ||
	    (dev->msix_enabled && type != DEXT_PCI_IRQ_MSIX) ||
	    (dev->msi_enabled && type != DEXT_PCI_IRQ_MSI))
		return -19;
#else
	(void)dev;
#endif
	return (int)vec; /* vector number == legacy irq in the shim */
}

/* ---- reset / power ---- */
int pci_reset_bus(struct pci_dev *dev)
{
	(void)dev;
	return -95; /* no bridge reset under DriverKit */
}

int pci_reset_function(struct pci_dev *dev)
{
	if (!dev) return -EINVAL;
#ifdef LINUXU_DEXT_DK
	/* The provider owns FLR and admission against DMA/IRQ/client lifetime. */
	return dext_pci_function_reset();
#else
	return -EOPNOTSUPP;
#endif
}

int pci_reset_slot(struct pci_dev *dev)
{
	return pci_reset_function(dev);
}

void pci_set_power_state(struct pci_dev *dev, int state)
{
#ifdef LINUXU_DEXT_DK
	if (!dev)
		return;
	if (state == PCI_D0) {
		if (pci_dk_force_d0(dev))
			dev_warn(&dev->dev, "could not transition PCI device to D0\n");
	} else {
		dev_warn(&dev->dev, "PCI power state D%x unsupported\n", state);
	}
#else
	(void)dev; (void)state;
#endif
}

int pci_power_state(struct pci_dev *dev)
{
#ifdef LINUXU_DEXT_DK
	u16 pmcsr;
	int cap, r = pci_dk_find_capability(dev, PCI_CAP_ID_PM, &cap);
	if (r)
		return r;
	if (!cap)
		return PCI_D0;
	if (cap + PCI_PM_CTRL + sizeof(pmcsr) > 256) return -EIO;
	r = pci_read_config_word(dev, cap + PCI_PM_CTRL, &pmcsr);
	if (r) return r;
	if (pmcsr == UINT16_MAX) return -ENODEV;
	return pmcsr & PCI_PM_CTRL_STATE_MASK;
#else
	(void)dev;
	return PCI_D0;
#endif
}

int pci_probe_reset_bus(struct pci_dev *dev)
{
	return pci_reset_bus(dev);
}

int pci_slot_reset(struct pci_dev *dev)
{
	return pci_reset_function(dev);
}

int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
	/* Match the working endpoint's bounded Transaction Pending drain. */
	for (unsigned elapsed = 0; elapsed < 1000; ++elapsed) {
		u16 status = UINT16_MAX;
		if (pcie_capability_read_word(dev, PCI_EXP_DEVSTA, &status) ||
		    status == UINT16_MAX) return 0;
		if (!(status & (1u << 5))) return 1;
		msleep(1);
	}
	return 0;
}

struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
	struct pci_saved_state *state = NULL;
	if (!dev) return NULL;
	pthread_mutex_lock(&pci_state_lock);
	if (dev->state_saved) {
		state = kmalloc(sizeof(*state), GFP_KERNEL);
		if (state) state->state = dev->saved_state;
	}
	pthread_mutex_unlock(&pci_state_lock);
	return state;
}

int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state)
{
	if (!dev || !state) return -EINVAL;
	if (state->state.header[0] != ((u32)dev->device << 16 | dev->vendor))
		return -ENODEV;
	pthread_mutex_lock(&pci_state_lock);
	dev->saved_state = state->state;
	memcpy(dev->saved_config_space, state->state.header, sizeof(state->state.header));
	dev->state_saved = true;
	pthread_mutex_unlock(&pci_state_lock);
	return 0;
}

/* ---- capability discovery ---- */
int pci_find_ext_capability(struct pci_dev *dev, int cap)
{
#ifdef LINUXU_DEXT_DK
	u32 header;
	unsigned int pos = 0x100;
	unsigned int limit = dev && dev->cfg_size > 0 ?
		(unsigned int)dev->cfg_size : 4096;
	unsigned int visited = 0;

	if (!dev || cap <= 0 || cap > UINT16_MAX || limit < 0x104)
		return 0;
	if (limit > 4096)
		limit = 4096;
	while (pos >= 0x100 && pos <= limit - 4 && visited++ < 1024) {
		if (pci_read_config_dword(dev, (int)pos, &header) ||
		    !header || header == UINT32_MAX)
			return 0;
		if ((header & 0xffffu) == (unsigned int)cap)
			return (int)pos;
		pos = (header >> 20) & 0xffcu;
	}
	return 0;
#else
	(void)dev; (void)cap;
	return 0;
#endif
}

int pci_find_capability(struct pci_dev *dev, int cap)
{
#ifdef LINUXU_DEXT_DK
	int found;
	return pci_dk_find_capability(dev, cap, &found) ? 0 : found;
#else
	(void)dev; (void)cap;
	return 0;
#endif
}

/* ---- enumeration ---- */
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
					    unsigned int devfn)
{
	(void)domain; (void)bus; (void)devfn;
	return NULL; /* single device: the dext's own IOPCIDevice */
}

struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *prev)
{
	(void)class;
	return prev ? NULL : NULL;
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
			       struct pci_dev *from)
{
	(void)vendor; (void)device;
	return from ? NULL : NULL;
}

void pci_dev_put(struct pci_dev *dev)
{
	if (dev)
		put_device(&dev->dev);
}

struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	if (dev)
		get_device(&dev->dev);
	return dev;
}

/* ---- driver registration ---- */
static pthread_mutex_t pci_driver_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pci_driver_idle = PTHREAD_COND_INITIALIZER;
static struct pci_driver *pci_registered_driver;
static struct pci_dev *pci_bound_device;
static struct pci_dev *pci_last_probe_device;
static int pci_last_probe_result = -EAGAIN;
/* Failed AMDGPU probe can leave a TTM device holding DMA without a bound
 * driver's remove callback. Such ownership cannot be revoked by devres. */
static struct pci_dev *pci_cleanup_retained_device;

int rt_pci_has_retained_probe(void)
{
	int retained;
	pthread_mutex_lock(&pci_driver_lock);
	retained = pci_cleanup_retained_device != NULL;
	pthread_mutex_unlock(&pci_driver_lock);
	return retained;
}

int rt_pci_probe_cleanup_retained(struct pci_dev *dev)
{
	int retained;
	pthread_mutex_lock(&pci_driver_lock);
	retained = dev && dev == pci_cleanup_retained_device;
	pthread_mutex_unlock(&pci_driver_lock);
	return retained;
}

int rt_pci_probe_result(struct pci_dev *dev)
{
	int result;
	if (!dev)
		return -ENODEV;
	pthread_mutex_lock(&pci_driver_lock);
	result = dev == pci_last_probe_device ? pci_last_probe_result : -EAGAIN;
	pthread_mutex_unlock(&pci_driver_lock);
	return result;
}
static int pci_driver_operation; /* 0 idle, 1 probe, 2 remove */

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
						struct pci_dev *dev)
{
	const struct pci_device_id *id;

	if (!ids || !dev)
		return NULL;
	for (id = ids; id->vendor || id->device || id->subsys_vendor ||
	     id->subsys_device || id->class || id->class_mask; id++) {
		if (id->vendor != PCI_ANY_ID && id->vendor != dev->vendor)
			continue;
		if (id->device != PCI_ANY_ID && id->device != dev->device)
			continue;
		if (id->subsys_vendor != PCI_ANY_ID &&
		    id->subsys_vendor != dev->subsystem_vendor)
			continue;
		if (id->subsys_device != PCI_ANY_ID &&
		    id->subsys_device != dev->subsystem_device)
			continue;
		if (((id->class ^ dev->class) & id->class_mask) == 0)
			return id;
	}
	return NULL;
}

int pci_register_driver(struct pci_driver *drv)
{
	struct pci_dev *dev;

	if (!drv || !drv->name || !drv->probe || !drv->id_table)
		return -EINVAL;
	pthread_mutex_lock(&pci_driver_lock);
	if (pci_registered_driver || pci_driver_operation) {
		pthread_mutex_unlock(&pci_driver_lock);
		return -EBUSY;
	}
	drv->driver.name = drv->name;
	pci_registered_driver = drv;
	dev = rt_device_active_pdev();
	pthread_mutex_unlock(&pci_driver_lock);
	/* Linux registration succeeds even when one device rejects probe. */
	if (dev)
		(void)pci_probe(dev);
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
	struct pci_dev *dev;

	if (!drv)
		return;
	pthread_mutex_lock(&pci_driver_lock);
	if (drv != pci_registered_driver) {
		pthread_mutex_unlock(&pci_driver_lock);
		return;
	}
	while (pci_driver_operation)
		pthread_cond_wait(&pci_driver_idle, &pci_driver_lock);
	if (pci_cleanup_retained_device) {
		dev = pci_cleanup_retained_device;
		pthread_mutex_unlock(&pci_driver_lock);
		dev_err(&dev->dev, "PCI unregister refused: failed-probe ownership retained\n");
		return;
	}
	pci_driver_operation = 2;
	pci_registered_driver = NULL;
	dev = pci_bound_device;
	pthread_mutex_unlock(&pci_driver_lock);
	/* device_remove: the driver's dev_groups go before its remove. */
	if (dev)
		device_remove_groups(&dev->dev, drv->dev_groups);
	if (dev && drv->remove)
		drv->remove(dev);
	if (dev)
		devres_release_all(&dev->dev);
	pthread_mutex_lock(&pci_driver_lock);
	if (dev) {
		pci_set_drvdata(dev, NULL);
		dev->dev.driver = NULL;
	}
	pci_bound_device = NULL;
	pci_last_probe_device = NULL;
	pci_last_probe_result = -EAGAIN;
	pci_driver_operation = 0;
	pthread_cond_broadcast(&pci_driver_idle);
	pthread_mutex_unlock(&pci_driver_lock);
}

int pci_probe(struct pci_dev *dev)
{
	struct pci_driver *drv;
	const struct pci_device_id *id;
	int ret, cleanup_retained = 0;

	if (!dev)
		return -EINVAL;
	pthread_mutex_lock(&pci_driver_lock);
	drv = pci_registered_driver;
	if (!drv || rt_device_active_pdev() != dev) {
		pthread_mutex_unlock(&pci_driver_lock);
		dev_err(&dev->dev, "PCI probe rejected: no registered driver or inactive device\n");
		return -ENODEV;
	}
	if (pci_driver_operation || pci_bound_device || pci_cleanup_retained_device) {
		pthread_mutex_unlock(&pci_driver_lock);
		return -EBUSY;
	}
	id = pci_match_id(drv->id_table, dev);
	if (!id) {
		pci_last_probe_device = dev;
		pci_last_probe_result = -ENODEV;
		pthread_mutex_unlock(&pci_driver_lock);
		dev_err(&dev->dev, "PCI probe: no match for %04x:%04x class %06x\n",
			dev->vendor, dev->device, dev->class);
		return -ENODEV;
	}
	pci_driver_operation = 1;
	pci_last_probe_device = dev;
	pci_last_probe_result = -EINPROGRESS;
	dev->dev.driver = &drv->driver;
	pthread_mutex_unlock(&pci_driver_lock);
	if (pci_device_is_present(dev)) {
		dev_info(&dev->dev, "PCI probe: entering upstream for %04x:%04x class %06x\n",
			 dev->vendor, dev->device, dev->class);
		ret = drv->probe(dev, id);
		dev_info(&dev->dev, "PCI probe: upstream returned %d\n", ret);
		if (ret && !strcmp(drv->name, "amdgpu")) {
			const int cleanup = rt_amdgpu_cleanup_failed_probe(dev);
			if (cleanup < 0) {
				cleanup_retained = 1;
				dev_err(&dev->dev, "PCI failed-probe cleanup refused (%d); retaining driver data and devres\n", cleanup);
			}
		} else if (!ret && drv->dev_groups) {
			/* really_probe: the driver's dev_groups once probe
			 * succeeded; failing them unbinds the driver again. */
			ret = device_add_groups(&dev->dev, drv->dev_groups);
			if (ret) {
				dev_err(&dev->dev, "device_add_groups() failed\n");
				if (drv->remove)
					drv->remove(dev);
			}
		}
	} else {
		dev_err(&dev->dev, "PCI probe: configuration identity unavailable\n");
		ret = -ENODEV;
	}
	if (ret && !cleanup_retained)
		devres_release_all(&dev->dev);
	pthread_mutex_lock(&pci_driver_lock);
	pci_last_probe_result = ret;
	if (cleanup_retained) {
		pci_cleanup_retained_device = dev;
	} else if (ret) {
		pci_set_drvdata(dev, NULL);
		dev->dev.driver = NULL;
	} else {
		pci_bound_device = dev;
	}
	pci_driver_operation = 0;
	pthread_cond_broadcast(&pci_driver_idle);
	pthread_mutex_unlock(&pci_driver_lock);
	return ret;
}

void pci_remove(struct pci_dev *dev)
{
	struct pci_driver *drv;

	if (!dev)
		return;
	pthread_mutex_lock(&pci_driver_lock);
	while (pci_driver_operation)
		pthread_cond_wait(&pci_driver_idle, &pci_driver_lock);
	if (dev == pci_cleanup_retained_device) {
		pthread_mutex_unlock(&pci_driver_lock);
		dev_err(&dev->dev, "PCI remove refused: failed-probe ownership retained\n");
		return;
	}
	if (pci_bound_device != dev) {
		if (pci_last_probe_device == dev) {
			pci_last_probe_device = NULL;
			pci_last_probe_result = -EAGAIN;
		}
		pthread_mutex_unlock(&pci_driver_lock);
		return;
	}
	drv = pci_registered_driver;
	pci_driver_operation = 2;
	pthread_mutex_unlock(&pci_driver_lock);
	if (drv)
		device_remove_groups(&dev->dev, drv->dev_groups);
	if (drv && drv->remove)
		drv->remove(dev);
	devres_release_all(&dev->dev);
	pthread_mutex_lock(&pci_driver_lock);
	pci_set_drvdata(dev, NULL);
	dev->dev.driver = NULL;
	pci_bound_device = NULL;
	pci_last_probe_device = NULL;
	pci_last_probe_result = -EAGAIN;
	pci_driver_operation = 0;
	pthread_cond_broadcast(&pci_driver_idle);
	pthread_mutex_unlock(&pci_driver_lock);
}

void pci_shutdown(struct pci_dev *dev)
{
	(void)dev;
}

int pci_resume(struct pci_dev *dev)
{
	(void)dev;
	return 0;
}

int pci_config_reset(struct pci_dev *dev)
{
	return pci_reset_function(dev);
}

void linuxu_pci_set_upstream_partner(u16 vendor, u16 device, u16 pcie_capabilities,
				     u32 link_capabilities)
{
	__atomic_store_n(&linuxu_partner_known, 0, __ATOMIC_RELEASE);
	memset(&linuxu_partner, 0, sizeof(linuxu_partner));
	linuxu_partner.vendor = vendor;
	linuxu_partner.device = device;
	linuxu_partner.is_pcie_device = 1;
	linuxu_partner.pcie_capable = 1;
	linuxu_partner.is_bridge = 1;
	linuxu_partner_pcie_caps = pcie_capabilities;
	linuxu_partner_link_caps = link_capabilities;
	__atomic_store_n(&linuxu_partner_known, 1, __ATOMIC_RELEASE);
}

void linuxu_pci_clear_upstream_partner(void)
{
	__atomic_store_n(&linuxu_partner_known, 0, __ATOMIC_RELEASE);
}

struct pci_dev *pci_upstream_bridge(struct pci_dev *dev)
{
	/* The endpoint's chain is the partner and then nothing: what lies
	 * beyond it (Thunderbolt's tunnel, the root port) has no registers a
	 * Linux caller could use. */
	if (!dev || pci_is_partner(dev) ||
	    !__atomic_load_n(&linuxu_partner_known, __ATOMIC_ACQUIRE))
		return NULL;
	return &linuxu_partner;
}

int pci_is_root_bus(struct pci_bus *bus)
{
#ifdef LINUXU_DEXT_DK
	return bus && bus->number == 0;
#else
	(void)bus;
	return 1;
#endif
}

void linuxu_pci_mark_removed(struct pci_dev *dev)
{
	if (dev)
		__atomic_store_n(&dev->error_state, pci_channel_io_perm_failure,
				 __ATOMIC_RELEASE);
}

int pci_device_is_present(struct pci_dev *dev)
{
	if (dev && __atomic_load_n(&dev->error_state, __ATOMIC_ACQUIRE) ==
		   pci_channel_io_perm_failure)
		return 0;
#ifdef LINUXU_DEXT_DK
	u32 identity = UINT32_MAX;
	return dev && dext_pci_config_read32(0, &identity) == 0 &&
		(uint16_t)identity == dev->vendor &&
		(uint16_t)(identity >> 16) == dev->device;
#else
	return dev != NULL;
#endif
}

int pci_dev_is_disconnected(struct pci_dev *dev)
{
	if (dev && __atomic_load_n(&dev->error_state, __ATOMIC_ACQUIRE) ==
		   pci_channel_io_perm_failure)
		return 1;
#ifdef LINUXU_DEXT_DK
	return !pci_device_is_present(dev);
#else
	return dev == NULL;
#endif
}

/* pci_map_rom is a header inline (2026 vendor pci.h); the runtime
 * definition below was dropped. */
void pci_unmap_rom(struct pci_dev *dev, void __iomem *rom)
{
	(void)dev; (void)rom;
}

int pci_resize_resource(struct pci_dev *dev, int bar, int size, int exclude_bars)
{
	(void)exclude_bars;
	if (!dev || bar < 0 || bar >= 6 || size < 0 || size >= 44)
		return -EINVAL;
	/* DriverKit owns the upstream bridge's allocation. Reusing the current
	 * aperture needs no writes; actual reallocation is unavailable here. */
	resource_size_t bytes = pci_rebar_size_to_bytes(size);
	if (bytes && bytes == pci_resource_len(dev, bar)) return 0;
	return -EOPNOTSUPP;
}

int pci_mmio_enabled(struct pci_dev *dev)
{
#ifdef LINUXU_DEXT_DK
	u16 cmd;
	return dev && !pci_read_config_word(dev, PCI_COMMAND, &cmd) &&
		(cmd & PCI_COMMAND_MEMORY);
#else
	return dev != NULL;
#endif
}

int pci_max_read_req_size(struct pci_dev *dev)
{
	(void)dev;
	return 128;
}

int pci_pcie_type(struct pci_dev *dev)
{
	u16 flags = 0;
	if (!dev || !dev->is_pcie_device ||
	    pcie_capability_read_word(dev, 2, &flags) || flags == UINT16_MAX)
		return PCI_EXP_TYPE_ENDPOINT;
	return (flags >> 4) & 15;
}

bool pcie_aspm_enabled(struct pci_dev *dev)
{
	/* The TB5 link is controlled by macOS, as in the reference driver.
	 * A negative errno here would convert to true in the upstream caller. */
	(void)dev;
	return false;
}

int pcie_get_mps(struct pci_dev *dev)
{
	u16 control;
	int result = pcie_capability_read_word(dev, 8, &control);
	if (result || control == UINT16_MAX) return result ? result : -ENODEV;
	unsigned encoding = (control >> 5) & 7;
	return encoding <= 5 ? 128 << encoding : -EINVAL;
}

static enum pci_bus_speed pci_link_speed(unsigned encoding)
{
	switch (encoding) {
	case 1: return PCIE_SPEED_2_5GT;
	case 2: return PCIE_SPEED_5_0GT;
	case 3: return PCIE_SPEED_8_0GT;
	case 4: return PCIE_SPEED_16_0GT;
	case 5: return PCIE_SPEED_32_0GT;
	case 6: return PCIE_SPEED_64_0GT;
	default: return PCI_SPEED_UNKNOWN;
	}
}

static enum pcie_link_width pci_link_width(unsigned width)
{
	switch (width) {
	case 1: case 2: case 4: case 8: case 12: case 16: case 32:
		return (enum pcie_link_width)width;
	default: return PCIE_LNK_WIDTH_UNKNOWN;
	}
}

enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev)
{
	u32 capability;
	if (pcie_capability_read_dword(dev, 0x0c, &capability) ||
	    capability == UINT32_MAX) return PCI_SPEED_UNKNOWN;
	return pci_link_speed(capability & 15);
}

enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev)
{
	u32 capability;
	if (pcie_capability_read_dword(dev, 0x0c, &capability) ||
	    capability == UINT32_MAX) return PCIE_LNK_WIDTH_UNKNOWN;
	return pci_link_width((capability >> 4) & 63);
}

u32 pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev,
			     enum pci_bus_speed *speed, enum pcie_link_width *width)
{
	/* DriverKit exposes this endpoint, not the complete Thunderbolt bridge
	 * path. Do not report endpoint line rate as end-to-end path bandwidth. */
	(void)dev;
	if (limiting_dev) *limiting_dev = NULL;
	if (speed) *speed = PCI_SPEED_UNKNOWN;
	if (width) *width = PCIE_LNK_WIDTH_UNKNOWN;
	return 0;
}

int pci_p2pdma_distance(struct pci_dev *orig, struct pci_dev *peer,
			bool write)
{
	(void)orig; (void)peer; (void)write;
	return 0;
}

/* Same validated, read-only v1 traversal as the working driver. Every
 * entry is checked before publishing a result, including nonmatching BARs. */
static int pci_rebar_read(struct pci_dev *dev, int bar, u64 *sizes)
{
	u32 header, first, capability, control;
	int pos, r;
	u32 seen = 0;
	u64 found = 0;
	if (!dev || !sizes || bar < 0 || bar >= 6) return -EINVAL;
	*sizes = 0;
	pos = pci_find_ext_capability(dev, 0x15);
	if (!pos) return -ENOENT;
	r = pci_read_config_dword(dev, pos, &header);
	if (!r) r = pci_read_config_dword(dev, pos + 8, &first);
	if (r) return r;
	if (header == UINT32_MAX || (header & 0xffff) != 0x15) return -EIO;
	if (((header >> 16) & 15) != 1) return -EOPNOTSUPP;
	unsigned count = (first >> 5) & 7;
	unsigned end = (unsigned)pos + 4 + count * 8;
	unsigned next = header >> 20;
	if (!count || count > 6 || end > 4096 ||
	    (next && ((next & 3) || next < 0x100 ||
	              (next >= (unsigned)pos && next < end)))) return -EIO;
	for (unsigned i = 0; i < count; ++i) {
		r = pci_read_config_dword(dev, pos + 4 + i * 8, &capability);
		if (!r) r = pci_read_config_dword(dev, pos + 8 + i * 8, &control);
		if (r) return r;
		unsigned index = control & 7, selected = (control >> 8) & 31;
		u32 supported = capability >> 4;
		if (capability == UINT32_MAX || control == UINT32_MAX || index >= 6 ||
		    (seen & (1u << index)) || selected >= 28 ||
		    !(supported & (1u << selected))) return -EIO;
		seen |= 1u << index;
		if (index == (unsigned)bar) found = supported;
	}
	if (!found) return -ENOENT;
	*sizes = found;
	return 0;
}

u64 pci_rebar_get_possible_sizes(struct pci_dev *dev, int bar)
{
	u64 sizes;
	return pci_rebar_read(dev, bar, &sizes) ? 0 : sizes;
}

bool pci_rebar_size_supported(struct pci_dev *dev, int bar, int size)
{
	return size >= 0 && size < 28 &&
		(pci_rebar_get_possible_sizes(dev, bar) & (1ULL << size));
}

int pci_rebar_get_max_size(struct pci_dev *dev, int bar)
{
	u64 sizes;
	int r = pci_rebar_read(dev, bar, &sizes);
	return r ? r : 63 - __builtin_clzll(sizes);
}

resource_size_t pci_rebar_size_to_bytes(int size)
{
	return size >= 0 && size < 44 ? 1ULL << (size + 20) : 0;
}

int pci_rebar_bytes_to_size(u64 bytes)
{
	/* PCIe encodes sizes as powers of two starting at one MiB.  Round
	 * non-powers up without truncating VRAM sizes larger than 4 GiB. */
	if (bytes <= (1ULL << 20))
		return 0;
	return 64 - __builtin_clzll(bytes - 1) - 20;
}

void pci_restore_msi_state(struct pci_dev *dev)
{
	(void)dev;
}

void pci_restore_config_space(struct pci_dev *dev)
{
	(void)dev;
}

int pci_error_detected(struct pci_dev *dev)
{
	(void)dev;
	return 0;
}

int pci_ignore_hotplug(struct pci_dev *dev)
{
	(void)dev;
	return 0;
}

int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u8 flags)
{
	(void)dev; (void)flags;
	return -95; /* no PCIe AtomicOp routing over TB5 */
}

int pci_wake_from_d3(struct pci_dev *dev, u32 flags)
{
	(void)dev; (void)flags;
	return -95;
}

int pci_pr3_present(struct pci_dev *dev)
{
	(void)dev;
	return 0;
}

struct pci_dev *pci_devices(void)
{
	return rt_device_active_pdev();
}

/* pci_bus_for_each_resource is a header macro (2026 vendor pci.h);
 * the old runtime function definition was dropped. */

/* ---- IRQ registration (routes to the rt irq list) ---- */
int request_irq(unsigned int irq, irq_handler_t handler,
		unsigned long flags, const char *name, void *dev_id)
{
	(void)irq; (void)handler; (void)flags; (void)name; (void)dev_id;
	return rt_pci_irq_request(NULL, irq, handler, flags, name, dev_id);
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler,
			 irq_handler_t thread_fn, unsigned long flags,
			 const char *name, void *dev_id)
{
	if (thread_fn) return -EOPNOTSUPP;
	return request_irq(irq, handler, flags,
			   name, dev_id);
}

void free_irq(unsigned int irq, void *dev_id)
{
	rt_pci_irq_free(NULL, irq, dev_id);
}
