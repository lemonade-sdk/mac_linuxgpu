/* linuxu shim: pdev_mmio — the fake-MMIO token table:
 * u32 token → {base ptr, size, memIndex};
 * readl/writel/readq/writeq/readb/writeb dispatch.
 *
 * Host backend (no GPU): a host shadow buffer per token so unit tests
 * work without hardware.  DriverKit backend (LINUXU_DEXT): IOPCIDevice
 * MemoryRead32/64/MemoryWrite32/64 — skeleton with TODOs where the
 * DriverKit specifics need verification. */
#include <rt/aperture.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/pci.h>
#include <linux/mm.h>
#include <rt/rt.h>
#ifdef LINUXU_DEXT_DK
#include <rt/dext_pci.h>
#include <rt/dext_dma.h>
#endif

/* ---- token slot table (shared with rt/); struct mmio_slot is
 * defined in linuxu/headers/linux/io.h (host-shadow readl/writel
 * path) so the inlines can dereference ->shadow. */
#ifndef _LINUXU_MMIO_SLOT_DEFINED
struct mmio_slot {
	int used;
	struct pci_dev *dev;
	uint8_t mem_index;
	uint64_t base;      /* GPU-visible physical base of the region  */
	uint64_t size;
	uint8_t *shadow;    /* host shadow buffer (NULL in dext)        */
	size_t shadow_len;  /* host: actual shadow backing (>= 16 KB)   */
};
#endif

static struct mmio_slot mmio_slots[RT_MMIO_NUM_SLOTS];
static pthread_mutex_t mmio_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef LINUXU_DEXT_DK
/* Synthetic pointers must never alias a different BAR after unmap.  Keep
 * retired token numbers recognizable so stale doorbell atomics fail closed.
 * The numbers are renewed when the last mapping is unmapped: the session's
 * register token lives from dext_open to dext_close, so an empty table means
 * no session, no upstream module and no synthetic pointer remain. */
static uint8_t mmio_issued[RT_MMIO_DK_MAX_SLOTS];
static unsigned int mmio_live;
#endif

#ifdef LINUXU_DEXT_DK
unsigned int rt_mmio_live_tokens(void)
{
	pthread_mutex_lock(&mmio_lock);
	unsigned int live = mmio_live;
	pthread_mutex_unlock(&mmio_lock);
	return live;
}

extern int dext_bar_info(uint8_t bar, uint8_t *mem_index, uint64_t *size);
extern int dext_mem_read8(uint32_t token, uint64_t off, uint8_t *value);
extern int dext_mem_read16(uint32_t token, uint64_t off, uint16_t *value);
extern int dext_mem_read32(uint32_t token, uint64_t off, uint32_t *value);
extern int dext_mem_read64(uint32_t token, uint64_t off, uint64_t *value);
extern int dext_mem_write8(uint32_t token, uint64_t off, uint8_t value);
extern int dext_mem_write16(uint32_t token, uint64_t off, uint16_t value);
extern int dext_mem_write32(uint32_t token, uint64_t off, uint32_t value);
extern int dext_mem_write64(uint32_t token, uint64_t off, uint64_t value);

int rt_mmio_slot_info(uint32_t token, uint8_t *mem_index, uint64_t *size)
{
	pthread_mutex_lock(&mmio_lock);
	if (!token || token >= RT_MMIO_NUM_SLOTS || !mmio_slots[token].used) {
		pthread_mutex_unlock(&mmio_lock);
		return -1;
	}
	if (mem_index) *mem_index = mmio_slots[token].mem_index;
	if (size) *size = mmio_slots[token].size;
	pthread_mutex_unlock(&mmio_lock);
	return 0;
}
#endif

#ifdef LINUXU_DEXT_DK
static uint32_t atomic_mmio_token(const volatile void *addr,
				  uint64_t *offset)
{
	uintptr_t raw = (uintptr_t)addr;
	uint64_t slot = rt_mmio_dk_token(raw);
	if (!slot || slot >= RT_MMIO_DK_MAX_SLOTS)
		return 0;
	pthread_mutex_lock(&mmio_lock);
	int issued = mmio_issued[slot];
	pthread_mutex_unlock(&mmio_lock);
	if (!issued) return 0;
	*offset = rt_mmio_dk_offset(raw);
	return (uint32_t)slot;
}

int linuxu_atomic64_mmio_read(const volatile void *addr, uint64_t *value)
{
	if (linuxu_aperture_contains(addr, sizeof(*value))) {
		linuxu_aperture_read(addr, value, sizeof(*value));
		return 1;
	}
	uint64_t offset;
	uint32_t token = atomic_mmio_token(addr, &offset);
	if (!token)
		return 0;
	*value = rt_mmio_readq(NULL, (void *)(uintptr_t)token, offset);
	return 1;
}

int linuxu_atomic64_mmio_write(volatile void *addr, uint64_t value)
{
	if (linuxu_aperture_contains(addr, sizeof(value))) {
		linuxu_aperture_write(addr, &value, sizeof(value));
		return 1;
	}
	uint64_t offset;
	uint32_t token = atomic_mmio_token(addr, &offset);
	if (!token)
		return 0;
	rt_mmio_writeq(NULL, (void *)(uintptr_t)token, offset, value);
	return 1;
}
#endif

/* linuxu: host-shadow readl/writel path (linuxu/headers/linux/io.h)
 * resolves the token's slot so the shadow can be dereferenced as a
 * real host buffer.  Returns the slot if the token is live AND has a
 * host shadow; out-of-span offsets get *off clamped to 0 and the
 * caller sees the first word (the token itself) instead of faulting. */
const struct mmio_slot *rt_mmio_slot_lookup(uint32_t token, size_t *off)
{
	const struct mmio_slot *s;

	if (token == 0 || token >= RT_MMIO_NUM_SLOTS)
		return NULL;
	s = &mmio_slots[token];
	if (!s->used || !s->shadow)
		return NULL;
	if (*off >= s->shadow_len)
		*off = 0;
	return s;
}

uint32_t rt_mmio_mint_token(struct pci_dev *dev, uint8_t mem_index,
			    uint64_t base, uint64_t size, int host_shadow)
{
	uint32_t token;

#ifdef LINUXU_DEXT_DK
	if (!size || size > RT_MMIO_DK_TOKEN_STRIDE || !dev)
#else
	if (!size)
#endif
		return RT_MMIO_TOKEN_INVALID;
	pthread_mutex_lock(&mmio_lock);
	for (token = 1; token <
#ifdef LINUXU_DEXT_DK
	     RT_MMIO_DK_MAX_SLOTS
#else
	     RT_MMIO_NUM_SLOTS
#endif
	     ; token++) {
		if (!mmio_slots[token].used
#ifdef LINUXU_DEXT_DK
		    && !mmio_issued[token]
#endif
		   ) {
			mmio_slots[token].used = 1;
#ifdef LINUXU_DEXT_DK
			mmio_issued[token] = 1;
			mmio_live++;
#endif
			mmio_slots[token].dev = dev;
			mmio_slots[token].mem_index = mem_index;
			mmio_slots[token].base = base;
			mmio_slots[token].size = size;
			mmio_slots[token].shadow = NULL;
			mmio_slots[token].shadow_len = 0;
			if (host_shadow) {
				/* T1: back the slot's shadow with the FULL
				 * resource length (rounded up to a 16 KB
				 * page) so a token issued for a 16 GB
				 * BAR0 / 512 KB BAR5 region can be read
				 * and written across its whole span —
				 * the driver's GART page-table init writes
				 * into BAR0 well past one 16 KB granule.
				 * Cap at 256 MB per slot (the fake-MMIO
				 * window size) to keep test RSS sane; the
				 * dext backend services from IOPCIDevice
				 * and never allocates a shadow. */
				/* Cap before rounding so a very large resource cannot
				 * wrap the allocation length back to one small page. */
				size_t len = size > RT_MMIO_REGION_SIZE ?
					RT_MMIO_REGION_SIZE : (size_t)size;
				len = (len + RT_MMIO_SLOT_GRANULE - 1) &
					~(size_t)(RT_MMIO_SLOT_GRANULE - 1);
				mmio_slots[token].shadow =
					calloc(1, len ? len : RT_MMIO_SLOT_GRANULE);
				mmio_slots[token].shadow_len =
					len ? len : RT_MMIO_SLOT_GRANULE;
				if (!mmio_slots[token].shadow) {
#ifdef LINUXU_DEXT_DK
					mmio_live--;
#endif
					memset(&mmio_slots[token], 0, sizeof(mmio_slots[token]));
					pthread_mutex_unlock(&mmio_lock);
					return RT_MMIO_TOKEN_INVALID;
				}
				/* first word carries the token itself */
				*(uint32_t *)mmio_slots[token].shadow = token;
			}
			pthread_mutex_unlock(&mmio_lock);
			return token;
		}
	}
	pthread_mutex_unlock(&mmio_lock);
	return RT_MMIO_TOKEN_INVALID;
}

void rt_mmio_free_token(uint32_t token)
{
	pthread_mutex_lock(&mmio_lock);
	if (token && token < RT_MMIO_NUM_SLOTS) {
#ifdef LINUXU_DEXT_DK
		if (mmio_slots[token].used && !--mmio_live)
			memset(mmio_issued, 0, sizeof(mmio_issued));
#endif
		free(mmio_slots[token].shadow);
		memset(&mmio_slots[token], 0,
		       sizeof(mmio_slots[0]));
	}
	pthread_mutex_unlock(&mmio_lock);
}

static struct mmio_slot *slot_for(uint32_t token)
{
	if (!token || token >= RT_MMIO_NUM_SLOTS ||
	    !mmio_slots[token].used)
		return NULL;
	return &mmio_slots[token];
}

/* ---- pci_iomap hook used by pci_stub.c ---- */
void *rt_mmio_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	uint8_t mem_index;
	uint64_t base, size;

	if (!dev || bar < 0 || bar >= 6)
		return NULL;
#ifdef LINUXU_DEXT_DK
	if (bar < 0 || bar > 5 || dext_bar_info((uint8_t)bar,
					       &mem_index, &size) != 0)
		return NULL;
	if (maxlen && size > maxlen) size = maxlen;
	if (size > RT_MMIO_DK_TOKEN_STRIDE) return NULL;
	base = 0; /* DriverKit MemoryRead offsets are BAR relative. */
#else
	mem_index = (uint8_t)bar;
	base = pci_resource_start(dev, bar);
	size = pci_resource_len(dev, bar);
	if (maxlen && size > maxlen) size = maxlen;
	if (bar != 0 && size > 512 * 1024) size = 512 * 1024;

#endif
	uint32_t token = rt_mmio_mint_token(dev, mem_index, base, size,
#ifdef LINUXU_DEXT_DK
					    0);
#else
					    1 /* host shadow for tests */);
#endif
	if (token == RT_MMIO_TOKEN_INVALID)
		return NULL;
#ifdef LINUXU_DEXT_DK
	return (void *)(uintptr_t)rt_mmio_dk_address(token);
#else
	return (void *)(uintptr_t)token;
#endif
}

void rt_mmio_iounmap(struct pci_dev *dev, void *base)
{
	(void)dev;
	rt_mmio_free_token((uint32_t)(uintptr_t)
#ifdef LINUXU_DEXT_DK
			   rt_mmio_dk_token((uint64_t)(uintptr_t)base)
#else
			   base
#endif
			   );
}

static int shadow_ok(struct mmio_slot *s, uint64_t off, uint64_t len)
{
	return s && s->shadow && off <= s->size && len <= s->size - off &&
		off <= s->shadow_len && len <= s->shadow_len - off;
}

/* ================================================================== *
 * readl/writel family — host backend
 * ================================================================== */
uint32_t rt_mmio_readl(struct rt_device *dev, void *p, uint64_t off)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	/* A lost DriverKit mapping must not look like a valid zero register. */
	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
		return UINT32_MAX;
#else
		return 0;
#endif
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	uint32_t v = UINT32_MAX;
	return dext_mem_read32(token, off, &v) == 0 ? v : UINT32_MAX;
#else
	if (!shadow_ok(s, off, 4))
		return 0;
	uint32_t value;
	memcpy(&value, s->shadow + off, sizeof(value));
	return value;
#endif
}

void rt_mmio_writel(struct rt_device *dev, void *p, uint64_t off,
		    uint32_t v)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
#endif
		return;
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	(void)dext_mem_write32(token, off, v);
#else
	if (!shadow_ok(s, off, 4))
		return;
	memcpy(s->shadow + off, &v, sizeof(v));
#endif
}

uint64_t rt_mmio_readq(struct rt_device *dev, void *p, uint64_t off)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
		return UINT64_MAX;
#else
		return 0;
#endif
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	uint64_t v = UINT64_MAX;
	return dext_mem_read64(token, off, &v) == 0 ? v : UINT64_MAX;
#else
	if (!shadow_ok(s, off, 8))
		return 0;
	uint64_t value;
	memcpy(&value, s->shadow + off, sizeof(value));
	return value;
#endif
}

void rt_mmio_writeq(struct rt_device *dev, void *p, uint64_t off,
		    uint64_t v)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
#endif
		return;
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	(void)dext_mem_write64(token, off, v);
#else
	if (!shadow_ok(s, off, 8))
		return;
	memcpy(s->shadow + off, &v, sizeof(v));
#endif
}

uint8_t rt_mmio_readb(struct rt_device *dev, void *p, uint64_t off)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
		return UINT8_MAX;
#else
		return 0;
#endif
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	uint8_t v = UINT8_MAX;
	return dext_mem_read8(token, off, &v) == 0 ? v : UINT8_MAX;
#else
	if (!shadow_ok(s, off, 1))
		return 0;
	return *(volatile uint8_t *)(s->shadow + off);
#endif
}

void rt_mmio_writeb(struct rt_device *dev, void *p, uint64_t off,
		    uint8_t v)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	if (!s) {
#ifdef LINUXU_DEXT_DK
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
#endif
		return;
	}

	(void)dev;
#ifdef LINUXU_DEXT_DK
	(void)dext_mem_write8(token, off, v);
#else
	if (!shadow_ok(s, off, 1))
		return;
	*(volatile uint8_t *)(s->shadow + off) = v;
#endif
}

#ifdef LINUXU_DEXT_DK
uint16_t rt_mmio_readw(struct rt_device *dev, void *p, uint64_t off)
{
	uint16_t v = UINT16_MAX;
	uint32_t token = (uint32_t)(uintptr_t)p;
	(void)dev;
	if (slot_for(token)) (void)dext_mem_read16(token, off, &v);
	else dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
	return v;
}

void rt_mmio_writew(struct rt_device *dev, void *p, uint64_t off, uint16_t v)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	(void)dev;
	if (slot_for(token)) (void)dext_mem_write16(token, off, v);
	else dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
}
#endif

/* ---- bulk copy ---- */
void rt_mmio_memcpy_fromio(void *dst, void *p, uint64_t off, size_t len,
			   struct rt_device *dev)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	(void)dev;
#ifdef LINUXU_DEXT_DK
	uint64_t size;
	if (rt_mmio_slot_info(token, NULL, &size) || off > size || len > size - off) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
		memset(dst, 0xff, len);
		return;
	}
	uint8_t *d = dst;
	for (size_t i = 0; i < len; i++)
		d[i] = rt_mmio_readb(dev, p, off + i);
#else
	if (shadow_ok(s, off, len))
		memcpy(dst, s->shadow + off, len);
	else
		memset(dst, 0, len);
#endif
}

void rt_mmio_memcpy_toio(void *p, const void *src, uint64_t off, size_t len,
			 struct rt_device *dev)
{
	uint32_t token = (uint32_t)(uintptr_t)p;
	struct mmio_slot *s = slot_for(token);

	(void)dev;
#ifdef LINUXU_DEXT_DK
	uint64_t size;
	if (rt_mmio_slot_info(token, NULL, &size) || off > size || len > size - off) {
		dext_pci_transport_record_fault(DEXT_PCI_FAULT_MMIO, off);
		return;
	}
	const uint8_t *d = src;
	for (size_t i = 0; i < len; i++)
		rt_mmio_writeb(dev, p, off + i, d[i]);
#else
	if (shadow_ok(s, off, len))
		memcpy(s->shadow + off, src, len);
#endif
}
