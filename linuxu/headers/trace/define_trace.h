/* linuxu: EDITED (third_party/linux/include/trace/define_trace.h; stripped bpf/
 * rust tracepoint paths, TRACEPOINTS_ENABLED block, and the multi-read
 * include dance — the linuxu closure defines its tracepoints only in
 * CREATE_TRACE_POINTS passes that are never taken). */
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Trace files that want to automate creation of all tracepoints defined
 * in their file should include this file.
 *
 * In the linuxu userspace build the tracepoint mechanism is a no-op:
 * DEFINE_TRACE/DECLARE_TRACE expand to nothing, and the CREATE_TRACE_POINTS
 * pass (the one the kernel uses to *generate* the per-system tracepoint
 * C files) is never taken because nothing defines it before the include.
 * Keep the guard so a trace header that includes this file twice behaves
 * like the kernel one.
 */

#ifdef CREATE_TRACE_POINTS

/* Prevent recursion */
#undef CREATE_TRACE_POINTS

#include <linux/stringify.h>

#undef TRACE_EVENT
#define TRACE_EVENT(name, proto, args, tstruct, assign, print)	\
	DEFINE_TRACE(name, PARAMS(proto), PARAMS(args))

#undef TRACE_EVENT_CONDITION
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) \
	TRACE_EVENT(name,						\
		PARAMS(proto),						\
		PARAMS(args),						\
		PARAMS(tstruct),					\
		PARAMS(assign),						\
		PARAMS(print))

#undef TRACE_EVENT_FN
#define TRACE_EVENT_FN(name, proto, args, tstruct,		\
		assign, print, reg, unreg)			\
	DEFINE_TRACE_FN(name, reg, unreg, PARAMS(proto), PARAMS(args))

#undef TRACE_EVENT_FN_COND
#define TRACE_EVENT_FN_COND(name, proto, args, cond, tstruct,	\
		assign, print, reg, unreg)			\
	DEFINE_TRACE_FN(name, reg, unreg, PARAMS(proto), PARAMS(args))

#undef TRACE_EVENT_SYSCALL
#define TRACE_EVENT_SYSCALL(name, proto, args, struct, assign, print, reg, unreg) \
	DEFINE_TRACE_SYSCALL(name, reg, unreg, PARAMS(proto), PARAMS(args))

#undef TRACE_EVENT_NOP
#define TRACE_EVENT_NOP(name, proto, args, struct, assign, print)

#undef DEFINE_EVENT_NOP
#define DEFINE_EVENT_NOP(template, name, proto, args)

#undef DEFINE_EVENT
#define DEFINE_EVENT(template, name, proto, args) \
	DEFINE_TRACE(name, PARAMS(proto), PARAMS(args))

#undef DEFINE_EVENT_FN
#define DEFINE_EVENT_FN(template, name, proto, args, reg, unreg) \
	DEFINE_TRACE_FN(name, reg, unreg, PARAMS(proto), PARAMS(args))

#undef DEFINE_EVENT_PRINT
#define DEFINE_EVENT_PRINT(template, name, proto, args, print)	\
	DEFINE_TRACE(name, PARAMS(proto), PARAMS(args))

#undef DEFINE_EVENT_CONDITION
#define DEFINE_EVENT_CONDITION(template, name, proto, args, cond)	\
	DEFINE_EVENT(template, name, PARAMS(proto), PARAMS(args))

#undef DECLARE_TRACE
#define DECLARE_TRACE(name, proto, args)	\
	DEFINE_TRACE(name##_tp, PARAMS(proto), PARAMS(args))

#undef DECLARE_TRACE_CONDITION
#define DECLARE_TRACE_CONDITION(name, proto, args, cond)	\
	DEFINE_TRACE(name##_tp, PARAMS(proto), PARAMS(args))

#undef DECLARE_TRACE_EVENT
#define DECLARE_TRACE_EVENT(name, proto, args)	\
	DEFINE_TRACE(name, PARAMS(proto), PARAMS(args))

#undef DECLARE_TRACE_EVENT_CONDITION
#define DECLARE_TRACE_EVENT_CONDITION(name, proto, args, cond)	\
	DEFINE_TRACE(name, PARAMS(proto), PARAMS(args))

#undef TRACE_INCLUDE
#undef __TRACE_INCLUDE

#ifndef TRACE_INCLUDE_FILE
# define TRACE_INCLUDE_FILE TRACE_SYSTEM
# define UNDEF_TRACE_INCLUDE_FILE
#endif

#ifndef TRACE_INCLUDE_PATH
# define __TRACE_INCLUDE(system) <trace/events/system.h>
# define UNDEF_TRACE_INCLUDE_PATH
#else
# define __TRACE_INCLUDE(system) __stringify(TRACE_INCLUDE_PATH/system.h)
#endif

# define TRACE_INCLUDE(system) __TRACE_INCLUDE(system)

/* Let the trace headers be reread */
#define TRACE_HEADER_MULTI_READ

/* linuxu: the trace header (amdgpu_trace.h etc.) is already included by the
 * .c file (or the enclosing header) in the same TU. The upstream
 * `#include TRACE_INCLUDE(TRACE_INCLUDE_FILE)` re-reads it via a
 * ../../drivers/gpu/drm/... path that does not exist in this layout, so the
 * re-include is dropped; the tracepoint macros above already turned the
 * DEFINE_* events into the no-op forms. */


/* Make all open coded DECLARE_TRACE nops */
#undef DECLARE_TRACE
#define DECLARE_TRACE(name, proto, args)
#undef DECLARE_TRACE_CONDITION
#define DECLARE_TRACE_CONDITION(name, proto, args, cond)

#undef DECLARE_TRACE_EVENT
#define DECLARE_TRACE_EVENT(name, proto, args)
#undef DECLARE_TRACE_EVENT_CONDITION
#define DECLARE_TRACE_EVENT_CONDITION(name, proto, args, cond)

/*
 * Fallback: after the CREATE_TRACE_POINTS pass, any trace_X() call for a
 * tracepoint that was NOT defined via TRACE_EVENT/DECLARE_TRACE in the
 * trace header expands to this no-op. This covers tracepoints that the
 * vendor driver references but that were added after the trace header was
 * last updated (e.g. trace_amdgpu_vm_bo_mapping_enabled in amdgpu_vm.c).
 */
/*
 * Fallback: after the CREATE_TRACE_POINTS pass, any trace_X() call for a
 * tracepoint that was NOT defined via TRACE_EVENT/DECLARE_TRACE in the
 * trace header expands to this no-op. This covers tracepoints that the
 * vendor driver references but that were added after the trace header was
 * last updated (e.g. trace_amdgpu_vm_bo_mapping_enabled in amdgpu_vm.c).
 * Defined here (not inside #ifdef CREATE_TRACE_POINTS) so they are
 * visible in the normal compilation pass.
 */
#if defined(__GNUC__) || defined(__clang__)
#ifndef trace_amdgpu_vm_bo_mapping_enabled
#define trace_amdgpu_vm_bo_mapping_enabled(...) 0
#endif
#ifndef trace_amdgpu_vm_bo_cs_enabled
#define trace_amdgpu_vm_bo_cs_enabled(...) 0
#endif
#endif

/*
 * EDITED: the kernel includes <trace/trace_events.h>, <trace/perf.h> and
 * <trace/bpf_probe.h> under TRACEPOINTS_ENABLED here.  linuxu has no
 * ftrace/perf/bpf; those headers are deliberately not part of the shadow
 * tree, so the block is dropped.
 */

#undef TRACE_EVENT
#undef TRACE_EVENT_FN
#undef TRACE_EVENT_FN_COND
#undef TRACE_EVENT_SYSCALL
#undef TRACE_EVENT_CONDITION
#undef TRACE_EVENT_NOP
#undef DEFINE_EVENT_NOP
#undef DECLARE_EVENT_CLASS
#undef DEFINE_EVENT
#undef DEFINE_EVENT_FN
#undef DEFINE_EVENT_PRINT
#undef DEFINE_EVENT_CONDITION
#undef TRACE_HEADER_MULTI_READ
#undef DECLARE_TRACE
#undef DECLARE_TRACE_CONDITION
#undef DECLARE_TRACE_EVENT
#undef DECLARE_TRACE_EVENT_CONDITION

/* Only undef what we defined in this file */
#ifdef UNDEF_TRACE_INCLUDE_FILE
# undef TRACE_INCLUDE_FILE
# undef UNDEF_TRACE_INCLUDE_FILE
#endif

#ifdef UNDEF_TRACE_INCLUDE_PATH
# undef TRACE_INCLUDE_PATH
# undef UNDEF_TRACE_INCLUDE_PATH
#endif

/* We may be processing more files */
#define CREATE_TRACE_POINTS

#endif /* CREATE_TRACE_POINTS */
