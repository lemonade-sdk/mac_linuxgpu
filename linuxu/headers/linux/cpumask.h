/* linuxu: SHIM (third_party/linux/include/linux/cpumask.h) — single-node
 * userspace model; API surface matches the upstream layout the amdkfd
 * .c files exercise (cpumask_of_node / cpumask_weight / for_each_cpu). */
#ifndef __LINUX_CPUMASK_H
#define __LINUX_CPUMASK_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/smp.h>

#define NR_CPUS 1
#define NR_CPU_IDS 1
#define nr_cpumask_bits NR_CPU_IDS

static inline const struct cpumask *cpumask_of_node(int node)
{
	static const struct cpumask mask = { { 1 } };
	(void)node;
	return &mask;
}

static inline int cpumask_weight(const struct cpumask *mask)
{
	return hweight_long(mask->bits[0]);
}

static inline void cpumask_copy(struct cpumask *dst,
				const struct cpumask *src)
{
	dst->bits[0] = src->bits[0];
}

static inline void cpumask_and(struct cpumask *dst,
			       const struct cpumask *src1,
			       const struct cpumask *src2)
{
	dst->bits[0] = src1->bits[0] & src2->bits[0];
}

static inline void cpumask_or(struct cpumask *dst,
			      const struct cpumask *src1,
			      const struct cpumask *src2)
{
	dst->bits[0] = src1->bits[0] | src2->bits[0];
}

static inline void cpumask_clear(struct cpumask *mask)
{
	mask->bits[0] = 0;
}

static inline void cpumask_clear_cpu(int cpu, struct cpumask *mask)
{
	mask->bits[0] &= ~BIT(cpu);
}

static inline void cpumask_set_cpu(int cpu, struct cpumask *mask)
{
	mask->bits[0] |= BIT(cpu);
}

static inline int cpumask_first(const struct cpumask *mask)
{
	return 0;
}

#define for_each_cpu(cpu, mask) \
	for ((cpu) = 0; (cpu) < NR_CPUS; (cpu)++)


/* vendor 2026 cpumask.h surface (kfd_topology.c) */


/* vendor 2026 cpumask.h surface (kfd_topology.c) */

/* vendor 2026 cpumask.h surface (kfd_topology.c) */
#define cpu_none_mask ((struct cpumask *)0)
#define nr_cpu_ids 1
/* x86 topology surface (kfd_topology.c / amdgpu_device.c) — single-CPU host
 * model: cpu_data() returns a pointer to a static struct cpuinfo_x86. */
#include <asm/cpu_device_id.h>
static struct cpuinfo_x86 linuxu_cpuinfo_0;
#define cpu_data(cpu) linuxu_cpuinfo_0

#endif /* __LINUX_CPUMASK_H */
