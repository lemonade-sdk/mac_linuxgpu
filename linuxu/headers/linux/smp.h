/* linuxu: SHIM (third_party/linux/include/linux/smp.h)
 *
 * SMP primitives for the host build: single "cpu" (cpu 0); per-cpu
 * storage is plain static memory (no SMP in the host test harness).
 * Mirrors the UP (!CONFIG_SMP) surface of vendor linux/smp.h plus the
 * barrier/cpumask macros the AS-IS drm headers need.
 */
#ifndef __LINUX_SMP_H
#define __LINUX_SMP_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/bitops.h>

typedef void (*smp_call_func_t)(void *info);

struct cpumask {
	unsigned long bits[1];
};

extern struct cpumask __cpu_possible_mask;
extern struct cpumask __cpu_online_mask;
extern struct cpumask __cpu_present_mask;

#define cpu_possible_mask ((const struct cpumask *)&__cpu_possible_mask)
#define cpu_online_mask   ((const struct cpumask *)&__cpu_online_mask)
struct cpumask;
#define cpu_present_mask  ((const struct cpumask *)&__cpu_present_mask)

static inline int num_online_cpus(void) { return 1; }
static inline int num_possible_cpus(void) { return 1; }

static inline bool cpu_online(int cpu) { return cpu == 0; }
static inline bool cpu_possible(int cpu) { return cpu == 0; }
static inline bool cpu_present(int cpu) { return cpu == 0; }

static inline bool cpumask_test_cpu(int cpu, const struct cpumask *mask)
{
	return test_bit((unsigned long)cpu, mask->bits);
}

#define for_each_cpu(cpu, mask) \
	for ((cpu) = 0; (cpu) < 1; (cpu)++)

#define for_each_online_cpu(cpu)	for_each_cpu((cpu), cpu_online_mask)
#define for_each_possible_cpu(cpu)	for_each_cpu((cpu), cpu_possible_mask)

/* ---- CPU id ---- */
#define raw_smp_processor_id()		0
#define smp_processor_id()		0

/* ---- barriers (owner: linux/compiler.h defines smp_* as macros) ---- */
#ifndef smp_store_mb
#define smp_store_mb(p, v) do { WRITE_ONCE((p), (v)); smp_mb(); } while (0)
#endif

/* ---- per-cpu (host: plain variables) ---- */
#define this_cpu_ptr(p)		(p)
#define this_cpu_read(v)	(v)
#define this_cpu_write(v, x)	((v) = (x))

/* ---- cross-CPU (UP: run locally / no-op) ---- */
static inline void on_each_cpu(smp_call_func_t func, void *info, int wait)
{
	if (func)
		func(info);
}
#define smp_call_function(func, info, wait) \
	on_each_cpu(func, info, wait)
#define smp_send_stop()		do { } while (0)
static inline void smp_send_reschedule(int cpu) { }
#define smp_init()		do { } while (0)

#endif /* __LINUX_SMP_H */
