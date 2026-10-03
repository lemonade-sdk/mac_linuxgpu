/* linuxu: SHIM (third_party/linux/include/linux/cpufreq.h) — cpufreq API
 * surface for kfd_topology.c. Single-node userspace model: fixed 3 GHz
 * report so the unmodified .c compiles. */
#ifndef __LINUX_CPUFREQ_H
#define __LINUX_CPUFREQ_H

#include <linux/types.h>

struct cpufreq_policy;
struct cpufreq_freqs;
struct cpufreq_driver;
struct device;

struct cpufreq_freqs {
	struct cpufreq_policy *policy;
	unsigned int old;
	unsigned int new;
};

struct cpufreq_policy {
	unsigned int cpu;
	int related_cpus;
	unsigned int min;
	unsigned int max;
	unsigned int cur;
};

struct cpufreq_driver {
	int (*target)(unsigned int cpu, unsigned int freq,
		      unsigned int relation);
};

extern unsigned int cpufreq_kget_policy(struct cpufreq_policy **policy);
extern void cpufreq_kput_policy(struct cpufreq_policy *policy);

static inline unsigned int cpufreq_quick_get_max(int cpu)
{
	(void)cpu;
	return 3000; /* kHz — fixed userspace report */
}

static inline unsigned int cpufreq_kget_cpu_possible(struct cpufreq_policy *policy)
{
	(void)policy;
	return 1;
}

static inline unsigned int cpufreq_kget_cpu_allowed(struct cpufreq_policy *policy)
{
	(void)policy;
	return 1;
}

static inline unsigned int cpufreq_kget_policy_cur(struct cpufreq_policy *policy)
{
	return policy->cur;
}

#endif /* __LINUX_CPUFREQ_H */
