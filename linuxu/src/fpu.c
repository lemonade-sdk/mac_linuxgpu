/* Kernel FPU sections (linux/fpu.h).
 *
 * The FP/SIMD register file belongs to the running thread, and the OS
 * saves it on every context switch, so entering a section needs no state
 * save. What Linux does enforce is kept: a section disables preemption and
 * must not nest, and kernel_fpu_end() must pair with a begin on the same
 * task. The in-section flag is a per-CPU variable, which linuxu keeps per
 * task (linux/percpu.h). */
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/fpu.h>
#include <linux/percpu.h>
#include <linux/preempt.h>

static DEFINE_PER_CPU(bool, in_kernel_fpu);

void kernel_fpu_begin(void)
{
	preempt_disable();
	WARN_ON_ONCE(this_cpu_read(in_kernel_fpu));
	this_cpu_write(in_kernel_fpu, true);
}

void kernel_fpu_end(void)
{
	WARN_ON_ONCE(!this_cpu_read(in_kernel_fpu));
	this_cpu_write(in_kernel_fpu, false);
	preempt_enable();
}

bool linuxu_kernel_fpu_active(void)
{
	return this_cpu_read(in_kernel_fpu);
}
