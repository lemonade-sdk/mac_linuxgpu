/* linuxu shim: percpu — per-CPU variables collapse to plain globals;
 * strategy (NOOP: get_cpu/put_cpu return 0,
 * per_cpu/this_cpu unused).  Single-node shims for anything that did
 * land in the header set. */
#include <linux/types.h>

int get_cpu(void)
{
	return 0;
}

void put_cpu(int cpu)
{
	(void)cpu;
}

unsigned int smp_processor_id(void)
{
	return 0;
}

/* per_cpu_ptr / this_cpu_read macros may resolve to these accessors */
void *linuxu_percpu_ptr(void *base, unsigned long offset)
{
	return (void *)((char *)base + offset);
}

#include <asm/cpu_device_id.h>
struct cpuinfo_x86 linuxu_cpuinfo_0;
struct cpuinfo_x86 *const linuxu_cpuinfo_ptr[1] = { &linuxu_cpuinfo_0 };
