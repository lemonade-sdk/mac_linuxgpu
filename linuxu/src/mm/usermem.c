/* linuxu shim: usermem — user page pinning stubs
 * (ENOSYS tripwire for direct GUP; the live path
 * also rejected by hmm_range_fault until page ownership is implemented). */
#include <linux/mm.h>

/*
 * get_user_pages / pin_user_pages: 0 call sites in the compute set —
 * direct pinning is unavailable and returns an error without touching outputs.
 */
int get_user_pages_flags(struct mm_struct *mm, unsigned long start,
			 unsigned long end, unsigned int flags,
			 struct page **pages, int *locked)
{
	(void)mm; (void)start; (void)end; (void)flags;
	(void)pages; (void)locked;
	return -38;
}
int pin_user_pages(unsigned long start, unsigned long nr_pages,
		   unsigned int flags, struct page **pages,
		   struct page **spot)
{
	(void)start; (void)nr_pages; (void)flags;
	(void)pages; (void)spot;
	return -38;
}

/* vma_lookup, mmget/mmput and get_task_mm live in mm.c. */
