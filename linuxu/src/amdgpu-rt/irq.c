/* linuxu shim: irq — device irq list + linuxu_rt_inject_irq for
 * tests (MAPPING: TLS
 * in_interrupt flag in the handler).  The dext-side
 * IOInterruptSource hook is a TODO comment. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * The rt irq entry table lives in amdgpu-rt/device.c; this file owns
 * the *linux/irq.h*-side view: the global irq number space the KMD
 * allocates via pci_alloc_irq_vectors and the inject hook tests use.
 */
extern int rt_irq_register(struct rt_device *dev, int vector,
			   int (*handler)(int irq, void *arg),
			   const char *name, void *arg);
extern void rt_irq_unregister(struct rt_device *dev, int vector);
extern int linuxu_rt_inject_irq(int vector);

struct rt_device;

/*
 * The KMD's amdgpu_irq.c path: request_irq(irq, handler, flags, name,
 * dev_id) → pci stub → rt_pci_irq_request → rt_irq_register.  The
 * inject hook below is the test entry.
 */

/*
 * Dext-side hook (TODO): wire an IOInterruptDispatchSource per MSI-X
 * vector on the IOPCIDevice; the callback sets the TLS in_interrupt
 * flag (spinlock.c: linuxu_set_in_interrupt) around the handler
 * invocation and drains the IH ring.
 */
int linuxu_rt_irq_init(struct rt_device *dev, int nvec)
{
	(void)dev;
	(void)nvec;
	return 0; /* TODO(linuxu): IOInterruptSource setup */
}

void linuxu_rt_irq_fini(struct rt_device *dev)
{
	(void)dev;
}

/* re-export the inject hook under a linuxu_ name for tests */
int linuxu_rt_inject_irq_linuxu(int vector)
{
	return linuxu_rt_inject_irq(vector);
}
