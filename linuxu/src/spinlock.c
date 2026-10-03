/* linuxu shim: spinlock — see linuxu/src/sync.c (STUB-THREAD: spinlock =
 * pthread mutex, irq variants = same lock + dummy flags; 0 local_irq_*
 * sites in the set). This translation unit owns
 * the irq save/restore no-op family and the in_interrupt TLS flag that
 * the IRQ handler thread sets (WP11). */
#include <pthread.h>
#include <stdint.h>
#include <rt/rt.h>
#include <rt/fatal.h>

#include <linux/spinlock.h>
#include <linux/interrupt.h>

/* ---- local_irq_* : no-op (no irq masking exists in the dext) ---- */
/*
 * local_irq_save/restore/disable/enable and local_bh_* are header
 * inlines (linuxu/headers/linux/spinlock.h) in the current header
 * set — the definitions below would redefine them.  This file keeps
 * only the in_interrupt TLS + irq enable/disable state helpers.
 */
void local_irq_save(unsigned long flags)
{
	flags = 0;
}

/* ---- TLS in_interrupt flag ---- */
/* IRQ delivery and the default/worker queues execute concurrently. DriverKit
 * TLS keeps a handler's context from changing another thread's decisions. */
#ifdef LINUXU_DEXT_DK
extern int IOThreadLocalStorageKeyCreate(uint64_t *key);
extern int IOThreadLocalStorageSet(uint64_t key, const void *value);
extern void *IOThreadLocalStorageGet(uint64_t key);
static pthread_once_t irq_tls_once = PTHREAD_ONCE_INIT;
static uint64_t irq_tls_key;
static int irq_tls_ready;
static void irq_tls_init(void)
{
	irq_tls_ready = IOThreadLocalStorageKeyCreate(&irq_tls_key) == 0;
}
static void irq_tls_require(void)
{
	if (pthread_once(&irq_tls_once, irq_tls_init) || !irq_tls_ready)
		/* unknown execution context cannot be treated as safe */
		LINUXU_FATAL("in_interrupt TLS key unavailable");
}
#else
static __thread int tls_in_interrupt;
#endif

/*
 * in_interrupt() is a header inline (spinlock.h) in the current set;
 * the TLS below is still used by the rt IRQ path via
 * linuxu_set_in_interrupt / linuxu_in_interrupt.
 */

/* the rt irq dispatch layer calls this on the way in/out of the
 * IOInterruptDispatchSource handler thread */
void linuxu_set_in_interrupt(int in)
{
#ifdef LINUXU_DEXT_DK
	irq_tls_require();
	if (IOThreadLocalStorageSet(irq_tls_key, (void *)(uintptr_t)(in != 0)))
		LINUXU_FATAL("in_interrupt TLS store failed");
#else
	tls_in_interrupt = in;
#endif
}

bool in_irq(void)
{
#ifdef LINUXU_DEXT_DK
	irq_tls_require();
	return IOThreadLocalStorageGet(irq_tls_key) != NULL;
#else
	return tls_in_interrupt != 0;
#endif
}

bool irq_disabled(void)
{
	return false;
}

bool irq_enabled(void)
{
	return true;
}

int disable_irq(unsigned int irq)
{
	return rt_irq_disable(irq, true);
}

int disable_irq_nosync(unsigned int irq)
{
	return rt_irq_disable(irq, false);
}

void enable_irq(unsigned int irq)
{
	rt_irq_enable(irq);
}

void enable_irq_nosync(unsigned int irq)
{
	enable_irq(irq);
}

void synchronize_irq(unsigned int irq)
{
	rt_irq_synchronize(irq);
}

void ack_irq(unsigned int irq)
{
	(void)irq;
}

void mask_irq(unsigned int irq)
{
	(void)irq;
}

void unmask_irq(unsigned int irq)
{
	(void)irq;
}


void generic_handle_irq(unsigned int irq)
{
	(void)irq;
}
