/* linuxu shim: pci_irq_seam — the rt-side half of the pci_stub.c →
 * rt_irq_register/rt_irq_unregister seam (WP T1 host backend).
 *
 * The same symbols are defined in amdgpu-rt/device.c for the dext
 * build; this file exists so host unit tests that link pci_stub.o
 * without the amdgpu-rt objects (test_dma_dart) get a working seam
 * without dragging in the whole rt device stack.  On the host there
 * is no real IRQ line — the seam keeps a small table so
 * request_irq/free_irq round-trip and the test can assert both
 * directions (request succeeds, free clears it). */
#include <pthread.h>
#include <stdint.h>

#include <linux/irqreturn.h>
#include <linux/pci.h>
#include <rt/rt.h>

struct irq_seam_slot {
	int used;
	int irq;
	const char *name;
	rt_irq_handler_fn handler;
	void *dev_id;
};

#define IRQ_SEAM_SLOTS 16

static pthread_mutex_t irq_seam_lock = PTHREAD_MUTEX_INITIALIZER;
static struct irq_seam_slot irq_seam[IRQ_SEAM_SLOTS];

__attribute__((weak)) int rt_pci_irq_request(struct pci_dev *dev, unsigned int irq,
		       rt_irq_handler_fn handler, unsigned long flags,
		       const char *name, void *dev_id)
{
	(void)dev; (void)flags;
	if (!handler || irq > INT32_MAX) return -22;
	pthread_mutex_lock(&irq_seam_lock);
	for (int i = 0; i < IRQ_SEAM_SLOTS; i++) {
		if (irq_seam[i].used && irq_seam[i].irq == (int)irq) {
			pthread_mutex_unlock(&irq_seam_lock);
			return -1; /* double request */
		}
	}
	for (int i = 0; i < IRQ_SEAM_SLOTS; i++) {
		if (!irq_seam[i].used) {
			irq_seam[i].used = 1;
			irq_seam[i].irq = (int)irq;
			irq_seam[i].name = name;
			irq_seam[i].handler = handler;
			irq_seam[i].dev_id = dev_id;
			pthread_mutex_unlock(&irq_seam_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&irq_seam_lock);
	return -1; /* table full */
}

__attribute__((weak)) void rt_pci_irq_free(struct pci_dev *dev, unsigned int irq, void *dev_id)
{
	(void)dev;
	pthread_mutex_lock(&irq_seam_lock);
	for (int i = 0; i < IRQ_SEAM_SLOTS; i++) {
		if (irq_seam[i].used && irq_seam[i].irq == (int)irq &&
		    irq_seam[i].dev_id == dev_id) {
			irq_seam[i].used = 0;
			irq_seam[i].handler = NULL;
			irq_seam[i].dev_id = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&irq_seam_lock);
}
