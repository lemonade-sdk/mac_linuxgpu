/* struct mm_struct lifetime, mmap_lock and the VMA registry.
 *
 * Linux semantics: mm_users counts users of the address space, and the last
 * mmput runs the exit_mmap equivalent (notifier release, then every VMA's
 * close and file reference) before dropping the mm_count pin that users
 * collectively hold. mm_count keeps the structure itself; the last mmdrop
 * frees it together with its notifier subscription list. No host page
 * tables exist: a VMA only records a range plus the kernel-side backing the
 * uaccess backend (uaccess.c) reads and writes through.
 */
#include <pthread.h>
#include <stdlib.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/bug.h>
#include <linux/file.h>

/* Serializes task->mm changes against get_task_mm (Linux's task_lock). */
static pthread_mutex_t task_mm_lock = PTHREAD_MUTEX_INITIALIZER;

struct mm_struct *linuxu_mm_alloc(void)
{
	struct mm_struct *mm = calloc(1, sizeof(*mm));

	if (!mm)
		return NULL;
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	init_rwsem(&mm->mmap_lock);
	init_rwsem(&mm->linuxu_vma_lock);
	mm->mm_rb = RB_ROOT;
	mm->task_size = TASK_SIZE_MAX;
	return mm;
}

void mmgrab(struct mm_struct *mm)
{
	atomic_inc(&mm->mm_count);
}

static void __mmdrop(struct mm_struct *mm)
{
	WARN_ON_ONCE(mm->mmap || mm->map_count);
	if (mm->notifier_subscriptions)
		__mmu_notifier_subscriptions_destroy(mm);
	rwsem_destroy(&mm->mmap_lock);
	rwsem_destroy(&mm->linuxu_vma_lock);
	free(mm);
}

void mmdrop(struct mm_struct *mm)
{
	if (WARN_ON_ONCE(atomic_read(&mm->mm_count) <= 0))
		return;
	if (atomic_dec_and_test(&mm->mm_count))
		__mmdrop(mm);
}

struct mm_struct *mmget(struct mm_struct *mm)
{
	if (mm)
		atomic_inc(&mm->mm_users);
	return mm;
}

bool mmget_not_zero(struct mm_struct *mm)
{
	return mm && atomic_inc_not_zero(&mm->mm_users);
}

/* Unlink every VMA and release it as unmap_vmas/remove_vma would. */
static void exit_mmap(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	linuxu_mm_exit(mm);
	mmap_write_lock(mm);
	while ((vma = mm->mmap)) {
		linuxu_mm_remove_vma(mm, vma);
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		if (vma->vm_file)
			fput(vma->vm_file);
		vm_area_free(vma);
	}
	mmap_write_unlock(mm);
}

void mmput(struct mm_struct *mm)
{
	if (!mm)
		return;
	if (WARN_ON_ONCE(atomic_read(&mm->mm_users) <= 0))
		return;
	if (!atomic_dec_and_test(&mm->mm_users))
		return;
	exit_mmap(mm);
	mmdrop(mm);
}

/* Linux defers to a workqueue only to avoid sleeping in atomic context;
 * every linuxu caller may sleep. */
void mmput_async(struct mm_struct *mm)
{
	mmput(mm);
}

/* ---- task <-> mm ---- */

struct mm_struct *get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	if (!task || (task->flags & PF_KTHREAD))
		return NULL;
	pthread_mutex_lock(&task_mm_lock);
	mm = task->mm;
	if (mm)
		mmget(mm);
	pthread_mutex_unlock(&task_mm_lock);
	return mm;
}

/* Exchange task->mm under the get_task_mm lock (exit_mm, exec). */
struct mm_struct *linuxu_task_set_mm(struct task_struct *task,
				     struct mm_struct *mm)
{
	struct mm_struct *old;

	pthread_mutex_lock(&task_mm_lock);
	old = task->mm;
	task->mm = mm;
	task->active_mm = mm;
	pthread_mutex_unlock(&task_mm_lock);
	return old;
}

/* Upstream kernel/kthread.c: the worker adopts @mm, pinned by mm_count
 * while in use; the caller holds an mm_users reference across use/unuse.
 * Linux keeps the pin as a lazy active_mm until the next context switch;
 * linuxu threads never switch, so unuse drops it at once. Linux warns when
 * a non-kthread calls this; linuxu workqueue workers are plain shim
 * threads, so only adopting over an existing mm is reported. */
void kthread_use_mm(struct mm_struct *mm)
{
	struct task_struct *tsk = current;

	WARN_ON_ONCE(tsk->mm);
	mmgrab(mm);
	pthread_mutex_lock(&task_mm_lock);
	tsk->active_mm = mm;
	tsk->mm = mm;
	pthread_mutex_unlock(&task_mm_lock);
}

void kthread_unuse_mm(struct mm_struct *mm)
{
	struct task_struct *tsk = current;

	WARN_ON_ONCE(tsk->mm != mm);
	pthread_mutex_lock(&task_mm_lock);
	tsk->mm = NULL;
	tsk->active_mm = NULL;
	pthread_mutex_unlock(&task_mm_lock);
	mmdrop(mm);
}

/* ---- mmap_lock ---- */

void mmap_read_lock(struct mm_struct *mm) { down_read(&mm->mmap_lock); }
void mmap_read_unlock(struct mm_struct *mm) { up_read(&mm->mmap_lock); }
void mmap_write_lock(struct mm_struct *mm) { down_write(&mm->mmap_lock); }
void mmap_write_unlock(struct mm_struct *mm) { up_write(&mm->mmap_lock); }
bool mmap_read_trylock(struct mm_struct *mm) { return down_read_trylock(&mm->mmap_lock); }
int mmap_read_lock_killable(struct mm_struct *mm) { return down_read_killable(&mm->mmap_lock); }
int mmap_write_lock_killable(struct mm_struct *mm) { return down_write_killable(&mm->mmap_lock); }
void mmap_write_downgrade(struct mm_struct *mm) { downgrade_write(&mm->mmap_lock); }

void mmap_assert_locked(const struct mm_struct *mm)
{
	WARN_ON_ONCE(!rwsem_is_locked((struct rw_semaphore *)&mm->mmap_lock));
}

void mmap_assert_write_locked(const struct mm_struct *mm)
{
	long count = atomic_long_read((atomic_long_t *)&mm->mmap_lock.count);

	WARN_ON_ONCE(!(count & RWSEM_WRITER_LOCKED));
}

/* ---- VMA registry ---- */

struct vm_area_struct *vm_area_alloc(struct mm_struct *mm)
{
	struct vm_area_struct *vma = calloc(1, sizeof(*vma));

	if (vma)
		vma->vm_mm = mm;
	return vma;
}

void vm_area_free(struct vm_area_struct *vma)
{
	free(vma);
}

/* Linux unmaps [start, end) of the VMA's mm. Only whole registered VMAs
 * can be unmapped here; partial ranges would need VMA splitting. */
int vma_munmap(struct vm_area_struct *vma, unsigned long start,
	       unsigned long end)
{
	struct mm_struct *mm;

	if (!vma)
		return -EINVAL;
	mm = vma->vm_mm;
	if (!mm || linuxu_find_vma_locked(mm, vma->vm_start) != vma)
		return 0;	/* not registered: nothing is mapped */
	if (start != vma->vm_start || end != vma->vm_end)
		return -EINVAL;
	linuxu_mm_remove_vma(mm, vma);
	return 0;
}

/* First VMA with vm_end > addr, or NULL. Caller holds mmap_lock or
 * linuxu_vma_lock. */
struct vm_area_struct *linuxu_find_vma_locked(struct mm_struct *mm,
					      unsigned long addr)
{
	struct rb_node *node = mm->mm_rb.rb_node;
	struct vm_area_struct *found = NULL;

	while (node) {
		struct vm_area_struct *vma = rb_entry(node, struct vm_area_struct, vm_rb);

		if (vma->vm_end > addr) {
			found = vma;
			if (vma->vm_start <= addr)
				break;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return found;
}

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	return mm ? linuxu_find_vma_locked(mm, addr) : NULL;
}

struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma = find_vma(mm, addr);

	return vma && vma->vm_start <= addr ? vma : NULL;
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
		unsigned long start_addr, unsigned long end_addr)
{
	struct vm_area_struct *vma = find_vma(mm, start_addr);

	return vma && end_addr > vma->vm_start ? vma : NULL;
}

int linuxu_mm_insert_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	struct rb_node **link, *parent = NULL;
	struct vm_area_struct *prev = NULL, *next;

	if (!mm || !vma || vma->vm_start >= vma->vm_end ||
	    (vma->vm_start | vma->vm_end) & ~PAGE_MASK ||
	    vma->vm_end > mm->task_size)
		return -EINVAL;
	mmap_assert_write_locked(mm);
	down_write(&mm->linuxu_vma_lock);
	next = linuxu_find_vma_locked(mm, vma->vm_start);
	if (next && next->vm_start < vma->vm_end) {
		up_write(&mm->linuxu_vma_lock);
		return -EEXIST;
	}
	link = &mm->mm_rb.rb_node;
	while (*link) {
		struct vm_area_struct *cur = rb_entry(*link, struct vm_area_struct, vm_rb);

		parent = *link;
		if (vma->vm_start < cur->vm_start) {
			link = &parent->rb_left;
		} else {
			prev = cur;
			link = &parent->rb_right;
		}
	}
	rb_link_node(&vma->vm_rb, parent, link);
	rb_insert_color(&vma->vm_rb, &mm->mm_rb);
	vma->vm_mm = mm;
	vma->vm_prev = prev;
	vma->vm_next = next;
	if (prev)
		prev->vm_next = vma;
	else
		mm->mmap = vma;
	if (next)
		next->vm_prev = vma;
	mm->map_count++;
	up_write(&mm->linuxu_vma_lock);
	return 0;
}

void linuxu_mm_remove_vma(struct mm_struct *mm, struct vm_area_struct *vma)
{
	if (!mm || !vma || WARN_ON_ONCE(vma->vm_mm != mm))
		return;
	mmap_assert_write_locked(mm);
	down_write(&mm->linuxu_vma_lock);
	rb_erase(&vma->vm_rb, &mm->mm_rb);
	if (vma->vm_prev)
		vma->vm_prev->vm_next = vma->vm_next;
	else
		mm->mmap = vma->vm_next;
	if (vma->vm_next)
		vma->vm_next->vm_prev = vma->vm_prev;
	vma->vm_next = vma->vm_prev = NULL;
	mm->map_count--;
	up_write(&mm->linuxu_vma_lock);
}

/* ---- PFN mappings ----
 * Linux remap_pfn_range populates page tables for [addr, addr + size) with
 * consecutive PFNs and marks the VMA VM_IO | VM_PFNMAP. Without host page
 * tables the range is recorded instead; drivers map whole VMAs (KFD doorbell
 * and MMIO pages), and a range split across calls must continue the earlier
 * one. The VMA carries no kernel backing, so uaccess still faults on it, as
 * Linux uaccess does on a VM_IO mapping. */
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
		    unsigned long pfn, unsigned long size, pgprot_t prot)
{
	unsigned long offset;

	if (!vma || !size || (addr | size) & ~PAGE_MASK ||
	    addr < vma->vm_start || addr > vma->vm_end ||
	    size > vma->vm_end - addr)
		return -EINVAL;
	offset = addr - vma->vm_start;
	if (vma->linuxu_pfn_bytes) {
		/* Only a continuation of the recorded range is representable. */
		if (offset != vma->linuxu_pfn_bytes ||
		    pfn != vma->linuxu_pfn + (offset >> PAGE_SHIFT) ||
		    prot != vma->linuxu_pfn_prot)
			return -EINVAL;
	} else if (offset) {
		return -EINVAL;
	} else {
		vma->linuxu_pfn = pfn;
		vma->linuxu_pfn_prot = prot;
	}
	vma->linuxu_pfn_bytes = offset + size;
	vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	return 0;
}
