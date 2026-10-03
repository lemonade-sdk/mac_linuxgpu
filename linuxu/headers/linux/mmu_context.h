/* linuxu: SHIM (third_party/linux/include/linux/mmu_context.h) */
#ifndef __LINUX_MMU_CONTEXT_H
#define __LINUX_MMU_CONTEXT_H

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     void *cpu)
{
	(void)prev; (void)next; (void)cpu;
}

static inline void enter_lazy_tlb(struct mm_struct *mm, void *cpu)
{
	(void)mm; (void)cpu;
}

static inline void leave_lazy_tlb(void *cpu)
{
	(void)cpu;
}

#endif /* __LINUX_MMU_CONTEXT_H */
