/* linuxu: SHIM (third_party/linux/include/linux/stacktrace.h) */
#include <linux/math64.h>
#ifndef __LINUX_STACKTRACE_H
#define __LINUX_STACKTRACE_H

#include <linux/types.h>

#define MAX_STACK_TRACE		64

struct stack_trace {
	unsigned int nr_entries;
	unsigned int max_entries;
	unsigned long entries[MAX_STACK_TRACE];
	unsigned int skip_entries;
};

static inline void stack_trace_save(struct stack_trace *st,
				    unsigned int skipnr)
{
	(void)st; (void)skipnr;
}

static inline void stack_trace_print(struct stack_trace *st,
				     unsigned int skipnr)
{
	(void)st; (void)skipnr;
}

#endif /* __LINUX_STACKTRACE_H */
