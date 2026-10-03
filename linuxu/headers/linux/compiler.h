/* linuxu: EDITED (third_party/linux/include/linux/compiler.h + compiler_types.h)
 *
 * Foundation header: inlines compiler_types.h + asm/rwonce.h (asm-generic),
 * keeps READ_ONCE/WRITE_ONCE/barriers via compiler builtins, strips
 * ftrace/objtool/coverage/kcsan machinery, no-ops dynamic debug.
 */
#ifndef __LINUX_COMPILER_H
#define __LINUX_COMPILER_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/compiler_attributes.h>

/* ---- compiler_types (subset used by the driver) ---- */

#ifndef __always_inline
#define __always_inline		__attribute__((always_inline))
#endif
#ifndef __noinline
#define __noinline		__attribute__((noinline))
#endif
#define __likely(x)		__builtin_expect(!!(x), 1)
#define __unlikely(x)		__builtin_expect(!!(x), 0)

#ifndef likely
#define likely(x)		__builtin_expect(!!(x), 1)
#endif

#ifndef __always_unused
#define __always_unused		__attribute__((unused))
#endif
#ifndef unlikely
#define unlikely(x)		__builtin_expect(!!(x), 0)
#endif
#define likely_notrace(x)	likely(x)
#define unlikely_notrace(x)	unlikely(x)

/* Optimization barrier */
#ifndef barrier
#define barrier() __asm__ __volatile__("": : :"memory")
#endif
#define barrier_data(ptr) __asm__ __volatile__("": :"r"(ptr) :"memory")
#define barrier_before_unreachable() do { } while (0)

#define unreachable() do { __builtin_unreachable(); } while (0)

/* ---- loop macros ---- */
#define for_each_if(cond)	if (!(cond)) continue;

/* ---- asm/rwonce (asm-generic) ---- */
#define READ_ONCE(x) \
	(*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val) \
	(*(__volatile__ __typeof__(x) *)&(x) = (val))

/* DriverKit queues run concurrently on different host CPUs. Compiler
 * barriers alone cannot publish initialized fences or DMA ring contents. */
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic() smp_mb()
#define smp_read_barrier_depends() smp_rmb()
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)

/* ---- init/exit/alignment/section attributes ---- */
#ifndef __aligned
#define __aligned(x)		__attribute__((aligned(x)))
#endif
#ifndef __packed
#define __packed			__attribute__((packed))
#endif
#define __read_mostly
#define __ro_after_init
#define __write_mostly
#define __init
#define __initdata
#define __exit
#define __exitdata
#define __section(x)		__attribute__((section(x)))
#define __noreturn		__attribute__((noreturn))
#ifndef __weak
#define __weak			__attribute__((weak))
#endif

#define __attribute_noinline __noinline
#define __attribute_const__ __attribute__((const))
#define __attribute_pure__ __attribute__((pure))
/* Device-facing barriers use the outer-shareable domain on ARM64. SMP
 * fences above only need to order coherent host memory between CPUs. */
#if defined(__aarch64__)
#define mb() __asm__ __volatile__("dmb osh" ::: "memory")
#define rmb() __asm__ __volatile__("dmb oshld" ::: "memory")
#define wmb() __asm__ __volatile__("dmb oshst" ::: "memory")
#else
#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
/* Ordering between CPU and device accesses to coherent DMA memory, and the
 * barriers the non-relaxed MMIO accessors use (Linux arm64: dma_wmb() is
 * dmb oshst, __iormb() a load-ordering barrier; dmb oshld is equivalent). */
#ifndef dma_wmb
#if defined(__aarch64__)
#define dma_rmb() __asm__ __volatile__("dmb oshld" ::: "memory")
#define dma_wmb() __asm__ __volatile__("dmb oshst" ::: "memory")
#else
#define dma_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define dma_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
#endif
#define __attribute_unused__ __attribute__((unused))
#define __maybe_unused		__attribute__((unused))
#ifndef __must_check
#define __must_check		__attribute__((warn_unused_result))
#endif
#ifndef __cold
#define __cold			__attribute__((cold))
#endif
#ifndef __hot
#define __hot			__attribute__((hot))
#endif
#define no_sanitize(addr)
#define no_sanitize_address(addr)

/* ---- sparse tags: no-ops in userspace ---- */
#define __user
#define __kernel
#define __iomem
#define __rcu
#define __bitwise

#define __printf(a, b)		__attribute__((format(printf, a, b)))

/* ---- module machinery: no-ops ---- */
#define module_init(fn)
#define module_exit(fn)
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_SOFTDEP(x)

#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_NS(sym, ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)

#define module_param(name, type, perm)
#define module_param_named(name, val, type, perm)
#define module_param_string(name, str, len, perm)
#define module_param_cb(name, op, p, perm)
#define module_param_array(name, type, n, perm)
#define MODULE_PARM_DESC(name, desc)

/* ---- static_assert (C11, kept for driver use) ---- */
#define static_assert(cond, msg) _Static_assert(cond, msg)
#define _Static_assert(cond, msg) _Static_assert(cond, msg)

/* ---- dynamic debug: no-op (CONFIG_DYNAMIC_DEBUG=n) ---- */

#define define_dynamic_debug_metadata
#define dprintk(level, fmt, ...)		do { } while (0)
#define dev_dbg(dev, fmt, ...)			do { } while (0)
#define dev_dbg_ratelimited(dev, fmt, ...)	do { } while (0)


#ifndef __malloc
#define __malloc __attribute__((malloc))
#endif
#ifndef asmlinkage
#define asmlinkage
#endif
#ifndef noreturn
#define noreturn
#endif

/* C23 / Clang fallthrough attribute */
#ifndef fallthrough
#define fallthrough do { } while (0)
#endif
#ifndef cpu_relax
#define cpu_relax() do { } while (0)
#endif
#ifndef DECLARE_FLEX_ARRAY
#define DECLARE_FLEX_ARRAY(type, name) type name[]
#endif

#ifndef OPTIMIZER_HIDE_VAR
#define OPTIMIZER_HIDE_VAR(x) do { } while (0)
#endif
#endif /* __LINUX_COMPILER_H */
