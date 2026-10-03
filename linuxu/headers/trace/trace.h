/* linuxu: SHIM (tracepoint no-op layer for the userspace build) */
#ifndef _LINUXU_TRACEPOINT_H
#define _LINUXU_TRACEPOINT_H

/*
 * The linuxu userspace build has no ftrace infrastructure.  Tracepoints
 * (TRACE_EVENT/DEFINE_EVENT/DECLARE_TRACE in amdgpu_trace.h,
 * amdgpu_trace_points.c, drm_trace_points.c, ...) compile to nothing:
 * the trace_*() call sites expand to empty statements, and the
 * *_tracepoints.c files (which only *define* tracepoints) produce no
 * symbols.  This header is the anchor the linux/ chunk's <linux/tracepoint.h>
 * and any <trace/...> include resolves into for that purpose.
 */

#include <trace/define_trace.h>

/*
 * No-op tracepoint "registration" so that DEFINE_TRACE/DECLARE_TRACE
 * expansions (which reference these) link/compile to nothing.
 */
#define DEFINE_TRACE(name, proto, args)
#define DEFINE_TRACE_FN(name, reg, unreg, proto, args)
#define DEFINE_TRACE_SYSCALL(name, reg, unreg, proto, args)

#endif
