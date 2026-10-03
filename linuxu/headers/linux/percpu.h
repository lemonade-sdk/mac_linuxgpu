/* linuxu: SHIM (third_party/linux/include/linux/percpu.h, percpu-defs.h)
 *
 * Per-CPU variables for a userspace kernel. Linux gives the running task
 * exclusive use of "this CPU's" copy between preempt_disable() and
 * preempt_enable(): the task cannot migrate and nothing else runs on that
 * CPU. linuxu threads all run concurrently as CPU 0 and preempt_disable()
 * excludes nobody, so one shared copy would be raced. Each task therefore
 * owns its copy: the first this_cpu_*() access by a task copies the
 * definition's initial value into task-private storage, released with the
 * task (put_task_struct). Within a task this is exactly the Linux contract
 * (a task always sees the copy of the CPU it runs on while pinned).
 *
 * per_cpu()/per_cpu_ptr() name the definition's own storage (the boot
 * CPU's copy); no built source sums or inspects other CPUs' copies.
 */
#ifndef __LINUX_PERCPU_H
#define __LINUX_PERCPU_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/preempt.h>

#define DEFINE_PER_CPU(type, name)		__typeof__(type) name
#define DECLARE_PER_CPU(type, name)		extern __typeof__(type) name
#define DEFINE_PER_CPU_ALIGNED(type, name)	\
	__typeof__(type) name __attribute__((aligned(64)))
#define DECLARE_PER_CPU_ALIGNED(type, name)	extern __typeof__(type) name
#define DEFINE_PER_CPU_READ_MOSTLY(type, name)	__typeof__(type) name
#define DECLARE_PER_CPU_READ_MOSTLY(type, name)	extern __typeof__(type) name
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU_ALIGNED(type, name)
#define DECLARE_PER_CPU_SHARED_ALIGNED(type, name) DECLARE_PER_CPU_ALIGNED(type, name)

/* The calling task's copy of the per-CPU object at var (size bytes).
 * Implemented in linuxu/src/percpu.c. */
void *linuxu_this_cpu_ptr(const void *var, size_t size);

#define this_cpu_ptr(ptr) \
	((__typeof__(ptr))linuxu_this_cpu_ptr((ptr), sizeof(*(ptr))))
#define raw_cpu_ptr(ptr)		this_cpu_ptr(ptr)
#define per_cpu_ptr(ptr, cpu)		((void)(cpu), (ptr))
#define per_cpu(var, cpu)		(*per_cpu_ptr(&(var), cpu))

#define this_cpu_read(pcp)		(*this_cpu_ptr(&(pcp)))
#define this_cpu_write(pcp, val)	((void)(*this_cpu_ptr(&(pcp)) = (val)))
#define this_cpu_add(pcp, val)		((void)(*this_cpu_ptr(&(pcp)) += (val)))
#define this_cpu_sub(pcp, val)		this_cpu_add(pcp, -(__typeof__(pcp))(val))
#define this_cpu_inc(pcp)		this_cpu_add(pcp, 1)
#define this_cpu_dec(pcp)		this_cpu_sub(pcp, 1)
#define this_cpu_add_return(pcp, val)	(*this_cpu_ptr(&(pcp)) += (val))
#define this_cpu_sub_return(pcp, val)	this_cpu_add_return(pcp, -(__typeof__(pcp))(val))
#define this_cpu_inc_return(pcp)	this_cpu_add_return(pcp, 1)
#define this_cpu_dec_return(pcp)	this_cpu_sub_return(pcp, 1)

#define __this_cpu_read(pcp)		this_cpu_read(pcp)
#define __this_cpu_write(pcp, val)	this_cpu_write(pcp, val)
#define __this_cpu_add(pcp, val)	this_cpu_add(pcp, val)
#define __this_cpu_sub(pcp, val)	this_cpu_sub(pcp, val)
#define __this_cpu_inc(pcp)		this_cpu_inc(pcp)
#define __this_cpu_dec(pcp)		this_cpu_dec(pcp)
#define __this_cpu_inc_return(pcp)	this_cpu_inc_return(pcp)
#define __this_cpu_dec_return(pcp)	this_cpu_dec_return(pcp)
#define raw_cpu_read(pcp)		this_cpu_read(pcp)
#define raw_cpu_write(pcp, val)		this_cpu_write(pcp, val)

#define get_cpu_var(var)	(*({ preempt_disable(); this_cpu_ptr(&(var)); }))
#define put_cpu_var(var)	do { (void)&(var); preempt_enable(); } while (0)
#define get_cpu_ptr(var)	({ preempt_disable(); this_cpu_ptr(var); })
#define put_cpu_ptr(var)	do { (void)(var); preempt_enable(); } while (0)

#endif /* __LINUX_PERCPU_H */
