/* uaccess backend: user addresses resolve through current->mm's VMA registry.
 *
 * Each VMA may carry kernel-side backing (linuxu_kaddr or linuxu_kmap, see
 * linux/mm.h). A transfer walks the VMAs covering the range under
 * linuxu_vma_lock and stops at the first byte that has no VMA, lacks the
 * needed permission, or has no backing; like a Linux fault, the bytes before
 * it are transferred and the remainder is reported. No mm (a kernel thread
 * or a DriverKit thread outside any process) faults every byte.
 */
#include <string.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/rwsem.h>
#include <rt/task.h>

enum uaccess_op { UACCESS_READ, UACCESS_WRITE, UACCESS_CLEAR };

/* Kernel address of @addr within @vma, with the contiguous byte count. */
static void *vma_backing(struct vm_area_struct *vma, unsigned long addr,
			 unsigned long *avail)
{
	unsigned long len = vma->vm_end - addr;
	void *kaddr;

	if (vma->linuxu_kmap) {
		kaddr = vma->linuxu_kmap(vma, addr, &len);
		if (len > vma->vm_end - addr)
			len = vma->vm_end - addr;
	} else if (vma->linuxu_kaddr) {
		kaddr = (char *)vma->linuxu_kaddr + (addr - vma->vm_start);
	} else {
		kaddr = NULL;
	}
	*avail = kaddr ? len : 0;
	return kaddr;
}

/* Call @fn on each backed chunk of [uaddr, uaddr + n) in order. @fn returns
 * how many bytes of the chunk it consumed; fewer than offered, or *@stop
 * becoming true, ends the walk. Returns the number of bytes consumed. */
typedef unsigned long (*chunk_fn)(void *kaddr, unsigned long done,
				  unsigned long len, void *ctx);

static unsigned long uaccess_walk(unsigned long uaddr, unsigned long n,
				  unsigned long need, chunk_fn fn, void *ctx,
				  const bool *stop)
{
	struct task_struct *task = linuxu_current_task_peek();
	struct mm_struct *mm = task ? task->mm : NULL;
	unsigned long done = 0;

	if (!mm || !n)
		return 0;
	down_read(&mm->linuxu_vma_lock);
	while (done < n) {
		unsigned long addr = uaddr + done, avail, chunk, used;
		struct vm_area_struct *vma = linuxu_find_vma_locked(mm, addr);
		void *kaddr;

		if (!vma || vma->vm_start > addr || (vma->vm_flags & need) != need)
			break;
		kaddr = vma_backing(vma, addr, &avail);
		if (!kaddr || !avail)
			break;
		chunk = n - done < avail ? n - done : avail;
		used = fn(kaddr, done, chunk, ctx);
		done += used;
		if (used < chunk || (stop && *stop))
			break;
	}
	up_read(&mm->linuxu_vma_lock);
	return done;
}

static unsigned long chunk_read(void *kaddr, unsigned long done,
				unsigned long len, void *ctx)
{
	memcpy((char *)ctx + done, kaddr, len);
	return len;
}

static unsigned long chunk_write(void *kaddr, unsigned long done,
				 unsigned long len, void *ctx)
{
	memcpy(kaddr, (const char *)ctx + done, len);
	return len;
}

static unsigned long chunk_clear(void *kaddr, unsigned long done,
				 unsigned long len, void *ctx)
{
	(void)done; (void)ctx;
	memset(kaddr, 0, len);
	return len;
}

unsigned long linuxu_copy_from_user(void *to, const void __user *from,
				    unsigned long n)
{
	return n - uaccess_walk((unsigned long)from, n, VM_READ, chunk_read, to, NULL);
}

unsigned long linuxu_copy_to_user(void __user *to, const void *from,
				  unsigned long n)
{
	return n - uaccess_walk((unsigned long)to, n, VM_WRITE, chunk_write,
				(void *)from, NULL);
}

unsigned long linuxu_clear_user(void __user *to, unsigned long n)
{
	return n - uaccess_walk((unsigned long)to, n, VM_WRITE, chunk_clear, NULL, NULL);
}

/* String helpers: copy up to the NUL (strncpy_from_user) or count it
 * (strnlen_user). */
struct string_ctx {
	char *dst;		/* NULL: count only */
	bool terminated;
};

static unsigned long chunk_string(void *kaddr, unsigned long done,
				  unsigned long len, void *ctx)
{
	struct string_ctx *s = ctx;
	const char *src = kaddr;
	const char *nul = memchr(src, '\0', len);
	unsigned long take = nul ? (unsigned long)(nul - src) + 1 : len;

	if (s->dst)
		memcpy(s->dst + done, src, take);
	s->terminated = nul != NULL;
	return take;
}

/* Linux: the length of the copied string without its NUL; @count when no
 * NUL was found within @count bytes (dst is then unterminated); -EFAULT on
 * a fault before either. */
long strncpy_from_user(char *dst, const char __user *src, long count)
{
	struct string_ctx s = { .dst = dst };
	unsigned long done;

	if (count <= 0)
		return 0;
	if (!access_ok(src, 1))
		return -EFAULT;
	if (!access_ok(src, count))
		count = (long)(TASK_SIZE_MAX - (unsigned long)src);
	done = uaccess_walk((unsigned long)src, (unsigned long)count, VM_READ,
			    chunk_string, &s, &s.terminated);
	if (s.terminated)
		return (long)done - 1;
	if (done == (unsigned long)count)
		return count;
	return -EFAULT;
}

/* Linux: string length including the NUL; 0 on a fault; n + 1 (more than
 * @n) when no NUL is found within @n bytes. */
long strnlen_user(const char __user *src, long n)
{
	struct string_ctx s = { .dst = NULL };
	unsigned long done;

	if (n <= 0 || !access_ok(src, 1))
		return 0;
	if (!access_ok(src, n))
		n = (long)(TASK_SIZE_MAX - (unsigned long)src);
	done = uaccess_walk((unsigned long)src, (unsigned long)n, VM_READ,
			    chunk_string, &s, &s.terminated);
	if (s.terminated)
		return (long)done;
	if (done == (unsigned long)n)
		return n + 1;
	return 0;
}
