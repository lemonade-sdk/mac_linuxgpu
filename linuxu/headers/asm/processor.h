/* linuxu: SHIM (vendor/linux/arch/x86/include/asm/processor.h)
 *
 * Host-clang stand-in for <asm/processor.h>. Only what the KMD needs:
 * cpu_relax() (vendor mm/mutex/spinlock headers and amdgpu pm .c files
 * include it).
 */
#ifndef _ASM_PROCESSOR_H
#define _ASM_PROCESSOR_H

#include <linux/types.h>

#ifndef cpu_relax
static inline void cpu_relax(void)
{
}
#endif

#endif /* _ASM_PROCESSOR_H */
