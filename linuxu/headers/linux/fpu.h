/* linuxu: SHIM (third_party/linux/include/linux/fpu.h -> asm/fpu.h)
 *
 * Kernel FPU sections. Linux brackets FP/SIMD use in kernel code with
 * kernel_fpu_begin()/kernel_fpu_end() because the kernel does not own the
 * FP register file; the section saves the user state, disables preemption
 * and must not nest. linuxu code runs in an ordinary process, where the
 * FP/SIMD registers always belong to the running thread and the OS
 * preserves them across context switches, so no state is saved. The
 * section is still tracked per task (linux/percpu.h) so a nested begin or
 * an unbalanced end is reported as Linux would.
 */
#ifndef _LINUX_FPU_H
#define _LINUX_FPU_H

#include <linux/types.h>

static inline bool kernel_fpu_available(void)
{
	return true;
}

void kernel_fpu_begin(void);
void kernel_fpu_end(void);
/* True between kernel_fpu_begin() and kernel_fpu_end() on this task. */
bool linuxu_kernel_fpu_active(void);

#endif /* _LINUX_FPU_H */
