/* linuxu shim: kernel_api_stubs_incomplete — stubs for kernel API functions
 * whose declarations use INCOMPLETE struct types (struct mm_struct, struct
 * irq_domain). These must be defined in a translation unit where the structs
 * remain INCOMPLETE (matching the header declaration context), otherwise the
 * definition uses the COMPLETE type and the compiler reports "conflicting
 * types".
 *
 * This file includes ONLY <linux/types.h> + forward declarations, so the
 * structs stay incomplete. The host path never calls these; the real
 * implementations are the p3-hw-gate.
 */
#include <linux/types.h>
#include <linux/errno.h>

struct mm_struct;
struct irq_domain;

/* mmap locks are real rwsems in linuxu/src/mm/mm.c. */

/* irq (declared in <linux/interrupt.h> with incomplete struct irq_domain) */
void generic_handle_domain_irq(struct irq_domain *d, unsigned int irq)
{
	(void)d; (void)irq;
}

/* dynamic debug (variadic, declared in <linux/printk.h>) */
#include <stdarg.h>
int dynamic_pr_debug(const char *fmt, ...)
{
	(void)fmt;
	return 0;
}

/* dma_buf_dynamic_attach (declared in <linux/dma-buf.h>) */

/* DriverKit-compatible aligned allocation and timing live in dext_alloc.c
 * and dext_time.c. A static bump allocation cannot be freed, and success
 * without sleeping or filling the clock breaks AMDGPU timeout logic. */
#include <stddef.h>
#include <stdint.h>

int isascii(int c) { return (c & ~0x7f) == 0; }
int isdigit(int c) { return (c >= '0' && c <= '9'); }
int isgraph(int c) { return (c > 32 && c < 127); }
int isspace(int c)
{
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
}

/* DriverKit stdio compatibility is implemented in dext_stdio.c. */
