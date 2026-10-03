/* linuxu: SHIM (third_party/linux/include/linux/tracepoint.h)
 *
 * Tracepoint API surface. The KMD's trace headers (amdgpu_trace.h,
 * gpu_scheduler_trace.h) use the standard two-pass
 * TRACE_INCLUDE / CREATE_TRACE_POINTS pattern, which drives
 * trace/define_trace.h (shadowed separately).
 *
 * With the default (no-CREATE_TRACE_POINTS) pass, TRACE_EVENT
 * expands to a no-op macro that only registers the tracepoint
 * prototype. Runtime probe dispatch: linuxu/src/log/tracepoint.c
 * (no-op stubs — we don't emit trace data).
 */
#ifndef _TRACEPOINT_H
#define _TRACEPOINT_H

#include <linux/types.h>
#include <stdarg.h>

struct tracepoint;
struct tracepoint_class;
struct notifier_block;
struct module;

/* ---- tracepoint infrastructure (runtime: linuxu/src/log/tracepoint.c) ---- */
extern void __tracepoint_func(void *priv);
extern int tracepoint_probe_register(struct tracepoint *tp,
				     void *probe, void *data);
extern int tracepoint_probe_register_prio(struct tracepoint *tp,
					  void *probe, void *data,
					  int prio);
extern int tracepoint_probe_register_may_exist(struct tracepoint *tp,
					 void *probe, void *data);
extern int tracepoint_probe_unregister(struct tracepoint *tp,
				       void *probe, void *data);
extern void for_each_kernel_tracepoint(
		void (*fct)(struct tracepoint *tp, void *priv),
		void *priv);
extern int register_tracepoint_module_notifier(struct notifier_block *nb);
extern int unregister_tracepoint_module_notifier(struct notifier_block *nb);

/* ---- static inline probe registration helpers ---- */
static inline int static_call_update(int *ptr) { return 0; }



/*
 * _enabled() probes for tracepoints: the shim emits no trace data, so the
 * probe functions are cheap static no-ops returning false (call sites like
 * `if (trace_foo_enabled()) trace_foo(...)` compile to a taken-never branch).
 * The static inline is declared after each event's trace_##name via a
 * trailing hook so it is visible at the call sites in the .c file.
 */
#define __linuxu_trace_enabled(name) \
__unused static inline bool trace_##name##_enabled(void) { return false; }

/* ---- TP_* shims (used by trace headers in the non-CREATE pass) ---- */
#ifndef TP_PROTO
#define TP_PROTO(args...) args

/* Fallback no-ops for tracepoints referenced by the vendor driver but
 * not declared in amdgpu_trace.h (added after the trace header was last
 * updated). These are safe to define unconditionally: the real trace_X()
 * macros from DEFINE_TRACE take precedence when CREATE_TRACE_POINTS is
 * defined, and these #ifndef guards prevent double-definition. */
#ifndef trace_amdgpu_vm_bo_mapping_enabled
#define trace_amdgpu_vm_bo_mapping_enabled(...) 0
#endif
#ifndef trace_amdgpu_vm_bo_cs_enabled
#define trace_amdgpu_vm_bo_cs_enabled(...) 0
#endif
#endif
#ifndef TP_ARGS
#define TP_ARGS(args...) args
#endif
#ifndef TP_STRUCT__entry
#define TP_STRUCT__entry(...)
#endif
#ifndef TP_fast_assign
#define TP_fast_assign
#endif
#ifndef TP_printk
#define TP_printk(fmt, ...)
#endif
#ifndef __field
#define __field(type, name)
#endif


#ifndef DECLARE_EVENT_CLASS
#define DECLARE_EVENT_CLASS(name, proto, args, ...) \
static inline void trace_##name(proto) \
{ \
} \
__linuxu_trace_enabled(name)
#endif
#ifndef TRACE_EVENT_PERF
#define TRACE_EVENT_PERF(name, proto, args, tstruct, assign, print) \
	TRACE_EVENT(name, proto, args, tstruct, assign, print)
#endif

/* ---- no-op TRACE_EVENT for the non-CREATE_TRACE_POINTS pass ---- */
#ifndef TRACE_EVENT
#define TRACE_EVENT(name, proto, args, ...) \
static inline void trace_##name(proto) \
{ \
} \
__linuxu_trace_enabled(name)
#endif
#ifndef TRACE_EVENT_CONDITION
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) \
	TRACE_EVENT(name, proto, args, tstruct, assign, print)
#endif
#ifndef TRACE_EVENT_FN
#define TRACE_EVENT_FN(name, proto, args, tstruct, assign, print, reg, unreg) \
	TRACE_EVENT(name, proto, args, tstruct, assign, print)
#endif
#ifndef DEFINE_EVENT
#define DEFINE_EVENT(template, name, proto, args) \
static inline void trace_##name(proto) \
{ \
}
#endif

#endif /* _TRACEPOINT_H */
