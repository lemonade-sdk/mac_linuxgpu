/* linuxu shim: device — amdgpu_device_ext lifecycle + fake-MMIO
 * token allocation
 * (MAPPING).  16 KB-aligned slots in the 256 MB
 * fake-MMIO region; host shadow buffers for tests. */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <rt/rt.h>
#include <rt/amdgpu_device_ext.h>
#include <rt/dart.h>
#ifdef LINUXU_DEXT_DK
#include <rt/dext_dma.h>
#endif
#ifdef LINUXU_DEXT_DK
#include <rt/dext_pci.h>
#endif
#include <linux/pci.h>
#include <linux/irqreturn.h>
#include <linux/interrupt.h>

/* errno values (errno.h may be absent in minimal host link) */
#ifndef EINVAL_L
#define EINVAL_L 22
#define ENOMEM_L 12
#define EBUSY_L 16
#endif

/* side hooks from pci/pdev_mmio.c */
extern uint32_t rt_mmio_mint_token(struct pci_dev *dev, uint8_t mem_index,
				   uint64_t base, uint64_t size,
				   int host_shadow);
extern void rt_mmio_free_token(uint32_t token);

/* per-adev ext table (one GPU in the dext; small open table) */
struct adev_ext_slot {
	struct amdgpu_device *adev;
	struct amdgpu_device_ext *dext;
};
#define ADEV_EXT_MAX 8
static struct adev_ext_slot adev_ext_slots[ADEV_EXT_MAX];
static pthread_mutex_t adev_ext_lock = PTHREAD_MUTEX_INITIALIZER;

struct amdgpu_device_ext *rt_amdgpu_device_alloc(struct amdgpu_device *adev)
{
	if (!adev)
		return NULL;
	struct amdgpu_device_ext *dext = calloc(1, sizeof(*dext));

	if (!dext)
		return NULL;
	dext->adev = adev;
	dext->ddev = NULL; /* set by the KMD glue once ddev exists */

	pthread_mutex_lock(&adev_ext_lock);
	int i;
	for (i = 0; i < ADEV_EXT_MAX; i++) {
		if (adev_ext_slots[i].adev == adev) {
			pthread_mutex_unlock(&adev_ext_lock);
			free(dext);
			return NULL;
		}
	}
	for (i = 0; i < ADEV_EXT_MAX; i++) {
		if (!adev_ext_slots[i].adev) {
			adev_ext_slots[i].adev = adev;
			adev_ext_slots[i].dext = dext;
			break;
		}
	}
	pthread_mutex_unlock(&adev_ext_lock);
	if (i == ADEV_EXT_MAX) {
		free(dext);
		return NULL;
	}
	return dext;
}

void rt_amdgpu_device_free(struct amdgpu_device_ext *dext)
{
	if (!dext)
		return;
	pthread_mutex_lock(&adev_ext_lock);
	int i;
	for (i = 0; i < ADEV_EXT_MAX; i++)
		if (adev_ext_slots[i].dext == dext) {
			adev_ext_slots[i].adev = NULL;
			adev_ext_slots[i].dext = NULL;
			break;
		}
	pthread_mutex_unlock(&adev_ext_lock);
	free(dext);
}

struct amdgpu_device_ext *rt_amdgpu_device_ext_get(
	struct amdgpu_device *adev)
{
	pthread_mutex_lock(&adev_ext_lock);
	int i;
	for (i = 0; i < ADEV_EXT_MAX; i++)
		if (adev_ext_slots[i].adev == adev)
			break;
	struct amdgpu_device_ext *dext =
		i < ADEV_EXT_MAX ? adev_ext_slots[i].dext : NULL;
	pthread_mutex_unlock(&adev_ext_lock);
	return dext;
}

int rt_amdgpu_device_init(struct amdgpu_device_ext *dext)
{
	struct pci_dev *pdev;
	uint64_t bar5_base, bar5_size, vram_base, vram_size;

	if (!dext || !dext->rt)
		return -EINVAL_L;
	if (dext->initialized)
		return -EBUSY_L;
	pdev = (struct pci_dev *)dext->rt; /* rt_device wraps the pdev */

	/* BAR5 (registers) fake-MMIO token — 16KB-aligned slot */
#ifdef LINUXU_DEXT_DK
	uint8_t bar5_memory_index = 0, bar0_memory_index = 0;
	bar5_base = pci_resource_start(pdev, 5);
	bar5_size = pci_resource_len(pdev, 5);
	/* The register window is the whole BAR as assigned; registers beyond
	 * it are reached by upstream's own indirect (PCIE index/data) path. */
	if (dext_bar_info(5, &bar5_memory_index, &bar5_size) != 0 ||
	    !bar5_base || !bar5_size)
		return -EINVAL_L;
#else
	bar5_base = 0; /* host tests: no device, a shadow-backed fake window */
	bar5_size = pci_resource_len(pdev, 5) ? pci_resource_len(pdev, 5)
					      : 512 * 1024;
#endif
	dext->mmio_token = rt_mmio_mint_token(pdev,
#ifdef LINUXU_DEXT_DK
					      bar5_memory_index,
#else
					      5,
#endif
					      bar5_base,
					      bar5_size,
#ifdef LINUXU_DEXT_DK
					      0);
#else
					      1);
#endif
	if (dext->mmio_token == RT_MMIO_TOKEN_INVALID)
		return -ENOMEM_L;
	dext->mmio_base = bar5_base;
	dext->mmio_size = bar5_size;

	/* BAR0 VRAM window token */
#ifdef LINUXU_DEXT_DK
	vram_base = pci_resource_start(pdev, 0);
	vram_size = pci_resource_len(pdev, 0);
	if (dext_bar_info(0, &bar0_memory_index, &vram_size) != 0 ||
	    !vram_base || !vram_size) {
		rt_mmio_free_token(dext->mmio_token);
		dext->mmio_token = 0;
		return -EINVAL_L;
	}
#else
	vram_base = 0;
	vram_size = 256 * 1024 * 1024; /* visible BAR0 window */
#endif
	dext->vram_token = rt_mmio_mint_token(pdev,
#ifdef LINUXU_DEXT_DK
					      bar0_memory_index,
#else
					      0,
#endif
					      vram_base,
					      vram_size,
#ifdef LINUXU_DEXT_DK
					      0);
#else
					      1);
#endif
	if (dext->vram_token == RT_MMIO_TOKEN_INVALID) {
		rt_mmio_free_token(dext->mmio_token);
		dext->mmio_token = 0;
		return -ENOMEM_L;
	}
	dext->vram_base = vram_base;
	dext->vram_size = vram_size;
	dext->vram_window = vram_size;

	dext->irq_vectors = 0;
	dext->initialized = true;
	return 0;
}

void rt_amdgpu_device_fini(struct amdgpu_device_ext *dext)
{
	if (!dext)
		return;
	if (dext->mmio_token)
		rt_mmio_free_token(dext->mmio_token);
	if (dext->vram_token)
		rt_mmio_free_token(dext->vram_token);
	dext->mmio_token = 0;
	dext->vram_token = 0;
	dext->initialized = false;
}

/* ---- IRQ registration (routes to rt/irq) ---- */
int rt_amdgpu_irq_register(struct amdgpu_device_ext *dext, int vector,
			   rt_irq_handler_t handler, const char *name,
			   void *arg)
{
	if (!dext || !dext->rt)
		return -EINVAL_L;
	return rt_irq_register(dext->rt, vector, handler, name, arg);
}

void rt_amdgpu_irq_unregister(struct amdgpu_device_ext *dext, int vector)
{
	if (dext && dext->rt)
		rt_irq_unregister(dext->rt, vector);
}

/* ---- host-testable rt_device lifecycle ---- */
struct rt_device {
	struct pci_dev pdev;   /* embedded: the dext's fake-PCI backend (T-dma-dart-dext fills BAR/config); minimal (zeroed) here */
	struct pci_bus bus;
	u64 dma_mask;
	int irq_vectors;
};

#ifdef LINUXU_DEXT_DK
static struct rt_device *rt_active_device;

static void rt_pci_device_release(struct device *device)
{
	struct pci_dev *pdev = container_of(device, struct pci_dev, dev);
	struct rt_device *rt = container_of(pdev, struct rt_device, pdev);
	free(rt);
}
#endif

struct rt_device *rt_device_alloc(void)
{
	struct rt_device *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->pdev.bus = &d->bus;
	d->pdev.dev.bus = &pci_bus_type;
	/* Linux's PCI defaults (pci_setup_device): 32-bit streaming and
	 * coherent masks until the driver negotiates its own width with
	 * dma_set_mask_and_coherent() (dma_mask.c), and no bus limit.  The
	 * host test backend's identity addresses are user pointers, which no
	 * 32-bit mask covers, so there the device starts unrestricted. */
	d->dma_mask =
#ifdef LINUXU_DEXT_DK
		DMA_BIT_MASK(32);
#else
		DMA_BIT_MASK(64);
#endif
	d->pdev.dev.dma_mask = &d->dma_mask;
	d->pdev.dev.coherent_dma_mask = d->dma_mask;
	d->pdev.dev.bus_dma_limit = 0;
#ifdef LINUXU_DEXT_DK
	struct dext_pci_snapshot pci;
	if (dext_pci_snapshot(&pci) != 0) {
		free(d);
		return NULL;
	}
	d->bus.number = pci.bus;
	d->pdev.devfn = PCI_DEVFN(pci.slot, pci.function);
	d->pdev.phfn = pci.function;
	d->pdev.domain = 0;
	d->pdev.vendor = pci.vendor;
	d->pdev.device = pci.device;
	d->pdev.subsystem_vendor = pci.subsystem_vendor;
	d->pdev.subsystem_device = pci.subsystem_device;
	d->pdev.class = pci.class_code;
	d->pdev.revision = pci.revision;
	d->pdev.cfg_size = 4096;
	d->pdev.is_pcie_device = 1;
	d->pdev.pcie_capable = 1;
	for (int bar = 0; bar < 6; bar++) {
		const struct dext_pci_bar *src = &pci.bar[bar];
		struct resource *dst = &d->pdev.resource[bar];
		if (!src->present) continue;
		dst->start = src->base;
		dst->end = src->base + src->size - 1;
		dst->name = "driverkit-pci-bar";
		dst->flags = (src->type & 1) ? IORESOURCE_IO : IORESOURCE_MEM;
		if (src->type & 0x08) dst->flags |= IORESOURCE_PREFETCH;
		if (src->type & 0x04) dst->flags |= IORESOURCE_MEM_64;
	}
	device_initialize(&d->pdev.dev);
	d->pdev.dev.release = rt_pci_device_release;
	/* pci_bus_type.dev_groups: vendor, device, link speed and width. */
	d->pdev.dev.groups = pci_dev_groups;
	if (dev_set_name(&d->pdev.dev, "%04x:%02x:%02x.%u", d->pdev.domain,
			 pci.bus, pci.slot, pci.function) || device_add(&d->pdev.dev)) {
		put_device(&d->pdev.dev);
		return NULL;
	}
#endif
#ifdef LINUXU_DEXT_DK
	__atomic_store_n(&rt_active_device, d, __ATOMIC_RELEASE);
#endif
	return d;
}

/* The rt device's fake-PCI handle (the adev's pdev).  NULL-safe. */
struct pci_dev *rt_device_get_pdev(struct rt_device *dev)
{
	return dev ? &dev->pdev : NULL;
}

/* The PCI device's sysfs directory (the amdgpu device directory Linux
 * tools read under /sys/class/drm/card0/device). NULL-safe. */
struct kobject *rt_device_kobject(struct rt_device *dev)
{
	return dev ? &dev->pdev.dev.kobj : NULL;
}

struct pci_dev *rt_device_active_pdev(void)
{
#ifdef LINUXU_DEXT_DK
	struct rt_device *dev = __atomic_load_n(&rt_active_device, __ATOMIC_ACQUIRE);
	return dev ? &dev->pdev : NULL;
#else
	return NULL;
#endif
}

void rt_device_free(struct rt_device *dev)
{
	if (!dev)
		return;
	/* pci_remove waits for an in-flight probe before inspecting retained
	 * ownership. Keep the active device visible until that decision is final. */
	pci_remove(&dev->pdev);
	if (rt_pci_probe_cleanup_retained(&dev->pdev)) {
		dev_err(&dev->pdev.dev, "runtime free refused: failed-probe ownership retained\n");
		return;
	}
#ifdef LINUXU_DEXT_DK
	struct rt_device *expected = dev;
	(void)__atomic_compare_exchange_n(&rt_active_device, &expected, NULL,
					  0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
#ifdef LINUXU_DEXT_DK
	devres_release_all(&dev->pdev.dev);
	device_unregister(&dev->pdev.dev);
#else
	free(dev);
#endif
}

void *rt_ioremap_active(uint64_t phys, uint64_t size)
{
#ifdef LINUXU_DEXT_DK
	return rt_ioremap(__atomic_load_n(&rt_active_device, __ATOMIC_ACQUIRE),
			  phys, size);
#else
	(void)phys;
	(void)size;
	return NULL;
#endif
}

void *rt_ioremap(struct rt_device *dev, uint64_t phys, uint64_t size)
{
#ifdef LINUXU_DEXT_DK
	if (!dev || !size || size > RT_MMIO_DK_TOKEN_STRIDE)
		return NULL;
	for (int bar = 0; bar < 6; bar++) {
		uint64_t start = pci_resource_start(&dev->pdev, bar);
		uint64_t length = pci_resource_len(&dev->pdev, bar);
		if (!length || phys < start || phys - start >= length ||
		    size > length - (phys - start))
			continue;
		/* VRAM BO kmap callers dereference this pointer directly. */
		if (bar == 0)
			return dext_bar0_cpu_map(phys - start, size);
		return pci_iomap_range(&dev->pdev, bar,
					(uintptr_t)(phys - start), (uintptr_t)size);
	}
	return NULL;
#else
	uint32_t token;

	if (!dev)
		return NULL;
	token = rt_mmio_mint_token(&dev->pdev, 5, phys, size, 1);
	if (token == RT_MMIO_TOKEN_INVALID)
		return NULL;
	return (void *)(uintptr_t)token;
#endif
}

void rt_mmio_free(void *cookie)
{
#ifdef LINUXU_DEXT_DK
	if (dext_bar0_cpu_unmap(cookie))
		return;
	rt_mmio_free_token((uint32_t)((uint64_t)(uintptr_t)cookie /
				       RT_MMIO_DK_TOKEN_STRIDE));
#else
	rt_mmio_free_token((uint32_t)(uintptr_t)cookie);
#endif
}

/* ---- DART buffers ---- */
void *rt_dart_alloc(struct rt_device *dev, size_t size, uint64_t *dma_addr)
{
	if (!dev || !dma_addr)
		return NULL;
	/* Share allocation ownership, alignment, and the global DMA budget
	 * with upstream dma_alloc_coherent users, including failed teardown. */
	return linuxu_dma_alloc_coherent(&dev->pdev.dev, size, dma_addr,
					GFP_KERNEL);
}

void rt_dart_free(struct rt_device *dev, void *cpu, size_t size,
		  uint64_t dma_addr)
{
	if (dev)
		linuxu_dma_free_coherent(&dev->pdev.dev, size, cpu, dma_addr);
}

uint64_t rt_dart_used(struct rt_device *dev)
{
	return dev ? linuxu_dart_used() : 0;
}

uint64_t rt_dart_peak(struct rt_device *dev)
{
	return dev ? linuxu_dart_peak() : 0;
}

/* ---- IRQ (host list; dext IOInterruptSource hook is a TODO) ---- */
struct rt_irq_entry {
	int vector;
	int used;
	rt_irq_handler_t handler;
	char name[64];
	void *arg;
	unsigned int disabled;
	unsigned int active;
	int retiring;
};
#define RT_IRQ_MAX 256
static struct rt_irq_entry rt_irq_entries[RT_IRQ_MAX];
static pthread_mutex_t rt_irq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rt_irq_idle = PTHREAD_COND_INITIALIZER;

int rt_irq_register(struct rt_device *dev, int vector,
		    rt_irq_handler_t handler, const char *name, void *arg)
{
	(void)dev;
	if (vector < 0 || vector >= RT_IRQ_MAX || !handler)
		return -EINVAL_L;
	pthread_mutex_lock(&rt_irq_lock);
	if (rt_irq_entries[vector].used || rt_irq_entries[vector].retiring) {
		pthread_mutex_unlock(&rt_irq_lock);
		return -EBUSY_L;
	}
	rt_irq_entries[vector].used = 1;
	rt_irq_entries[vector].vector = vector;
	rt_irq_entries[vector].handler = handler;
	rt_irq_entries[vector].arg = arg;
	if (name)
		snprintf(rt_irq_entries[vector].name,
			 sizeof(rt_irq_entries[vector].name), "%s", name);
	pthread_mutex_unlock(&rt_irq_lock);
	/* TODO(linuxu): IOInterruptSource on the MSI-X vector */
	return 0;
}

static void rt_irq_remove(int vector, void *arg, bool match_arg)
{
	if (vector < 0 || vector >= RT_IRQ_MAX)
		return;
	pthread_mutex_lock(&rt_irq_lock);
	struct rt_irq_entry *entry = &rt_irq_entries[vector];
	if (!entry->used || (match_arg && entry->arg != arg)) {
		pthread_mutex_unlock(&rt_irq_lock);
		return;
	}
	entry->used = 0;
	entry->retiring = 1;
	while (entry->active)
		pthread_cond_wait(&rt_irq_idle, &rt_irq_lock);
	memset(&rt_irq_entries[vector], 0, sizeof(rt_irq_entries[0]));
	pthread_mutex_unlock(&rt_irq_lock);
}

void rt_irq_unregister(struct rt_device *dev, int vector)
{
	(void)dev;
	rt_irq_remove(vector, NULL, false);
}

int rt_irq_disable(unsigned int vector, bool synchronize)
{
	if (vector >= RT_IRQ_MAX) return -EINVAL_L;
	pthread_mutex_lock(&rt_irq_lock);
	struct rt_irq_entry *entry = &rt_irq_entries[vector];
	if (!entry->used || entry->disabled == UINT32_MAX) {
		pthread_mutex_unlock(&rt_irq_lock);
		return -EINVAL_L;
	}
	entry->disabled++;
	while (synchronize && entry->active)
		pthread_cond_wait(&rt_irq_idle, &rt_irq_lock);
	pthread_mutex_unlock(&rt_irq_lock);
	return 0;
}

void rt_irq_enable(unsigned int vector)
{
	if (vector >= RT_IRQ_MAX) return;
	pthread_mutex_lock(&rt_irq_lock);
	struct rt_irq_entry *entry = &rt_irq_entries[vector];
	if (entry->used && entry->disabled) entry->disabled--;
	pthread_mutex_unlock(&rt_irq_lock);
}

void rt_irq_synchronize(unsigned int vector)
{
	if (vector >= RT_IRQ_MAX) return;
	pthread_mutex_lock(&rt_irq_lock);
	while (rt_irq_entries[vector].active)
		pthread_cond_wait(&rt_irq_idle, &rt_irq_lock);
	pthread_mutex_unlock(&rt_irq_lock);
}

int rt_irq_vector_count(struct rt_device *dev)
{
	return dev ? dev->irq_vectors : 0;
}

/* test hook: inject an IRQ to drain the list (unit tests) */
int linuxu_rt_inject_irq(int vector)
{
	int ret = 0;
	rt_irq_handler_t handler = NULL;
	void *arg = NULL;

	pthread_mutex_lock(&rt_irq_lock);
	if (vector >= 0 && vector < RT_IRQ_MAX &&
	    rt_irq_entries[vector].used && !rt_irq_entries[vector].disabled) {
		handler = rt_irq_entries[vector].handler;
		arg = rt_irq_entries[vector].arg;
		rt_irq_entries[vector].active++;
	}
	pthread_mutex_unlock(&rt_irq_lock);
	if (handler) {
		bool prior = in_irq();
		extern void linuxu_set_in_interrupt(int in);
		linuxu_set_in_interrupt(1);
		ret = handler(vector, arg);
		linuxu_set_in_interrupt(prior);
		pthread_mutex_lock(&rt_irq_lock);
		rt_irq_entries[vector].active--;
		pthread_cond_broadcast(&rt_irq_idle);
		pthread_mutex_unlock(&rt_irq_lock);
	}
	return ret;
}

/* ---- vram base/size accessors ---- */
uint64_t rt_vram_base(struct rt_device *dev)
{
	(void)dev;
	return 0;
}

uint64_t rt_vram_size(struct rt_device *dev)
{
	(void)dev;
	return 0; /* filled by rt_amdgpu_device_init from the ext */
}

uint64_t rt_vram_window(struct rt_device *dev)
{
	(void)dev;
	return 256 * 1024 * 1024;
}

/* ---- pci→rt irq bridge used by pci_stub.c (canonical prototype
 * with rt_irq_handler_fn in <rt/rt.h>; irq_handler_t is the same
 * (int, void*) -> int representation) ---- */
int rt_pci_irq_request(struct pci_dev *dev, unsigned int irq,
		       rt_irq_handler_fn handler, unsigned long flags,
		       const char *name, void *dev_id)
{
	(void)dev; (void)flags;
#ifdef LINUXU_DEXT_DK
	unsigned int armed = 0, type = DEXT_PCI_IRQ_NONE;
	if (dext_pci_irq_status(&armed, &type) || irq >= armed ||
	    type == DEXT_PCI_IRQ_NONE)
		return -19;
#endif
	return rt_irq_register(NULL, (int)irq, (rt_irq_handler_t)handler,
			       name, dev_id);
}

void rt_pci_irq_free(struct pci_dev *dev, unsigned int irq, void *dev_id)
{
	(void)dev;
	if (irq < RT_IRQ_MAX) rt_irq_remove((int)irq, dev_id, true);
}
