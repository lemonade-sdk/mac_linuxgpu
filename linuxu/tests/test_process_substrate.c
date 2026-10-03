/* linuxu process substrate: task swap, per-process descriptor tables, mmu
 * notifier get/put and exit order, the VMA registry and uaccess backend,
 * get_task_mm/kthread_use_mm, and SIGKILL waking interruptible waits. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <linux/anon_inodes.h>
#include <linux/dma-fence.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <rt/process.h>
#include <rt/task.h>

/* ---- event log shared by notifier and file callbacks ---- */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char events[32][24];
static unsigned int nevents;

static void log_event(const char *what)
{
	pthread_mutex_lock(&log_lock);
	assert(nevents < 32);
	snprintf(events[nevents++], sizeof(events[0]), "%s", what);
	pthread_mutex_unlock(&log_lock);
}

static int event_index(const char *what)
{
	for (unsigned int i = 0; i < nevents; i++)
		if (!strcmp(events[i], what))
			return (int)i;
	return -1;
}

/* ---- files ---- */
static int file_tag_a, file_tag_b;

static int file_release(struct inode *inode, struct file *file)
{
	(void)inode;
	log_event(file->private_data == &file_tag_a ? "file a" : "file b");
	return 0;
}

static const struct file_operations fops = { .release = file_release };

/* ---- notifiers ---- */
struct tracked_notifier {
	struct mmu_notifier mn;
	const char *name;
	bool put_on_release;
};

static unsigned int allocs, frees;

static void notifier_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
	struct tracked_notifier *t = container_of(mn, struct tracked_notifier, mn);
	char what[24];

	assert(mn->mm == mm);
	snprintf(what, sizeof(what), "release %s", t->name);
	log_event(what);
	/* KFD drops its notifier reference from inside ->release. */
	if (t->put_on_release)
		mmu_notifier_put(mn);
}

static void notifier_free(struct mmu_notifier *mn)
{
	struct tracked_notifier *t = container_of(mn, struct tracked_notifier, mn);
	char what[24];

	snprintf(what, sizeof(what), "free %s", t->name);
	log_event(what);
	frees++;
	free(t);
}

#define DEFINE_TRACKED_OPS(id, put)						\
static struct mmu_notifier *alloc_##id(struct mm_struct *mm)			\
{										\
	struct tracked_notifier *t = calloc(1, sizeof(*t));			\
	(void)mm;								\
	assert(t);								\
	t->name = #id;								\
	t->put_on_release = put;						\
	allocs++;								\
	return &t->mn;								\
}										\
static const struct mmu_notifier_ops ops_##id = {				\
	.release = notifier_release,						\
	.alloc_notifier = alloc_##id,						\
	.free_notifier = notifier_free,						\
};
DEFINE_TRACKED_OPS(a, true)
DEFINE_TRACKED_OPS(b, false)
DEFINE_TRACKED_OPS(c, false)

static struct mmu_notifier *alloc_fail(struct mm_struct *mm)
{
	(void)mm;
	return ERR_PTR(-ESRCH);
}
static const struct mmu_notifier_ops ops_fail = {
	.alloc_notifier = alloc_fail, .free_notifier = notifier_free,
};

/* ---- uaccess backing ---- */
#define UBASE (64 * PAGE_SIZE)
static unsigned char rw_backing[2 * PAGE_SIZE];
static unsigned char ro_backing[PAGE_SIZE];

/* A chunked backing: at most 100 contiguous bytes per lookup. */
static void *chunked_kmap(struct vm_area_struct *vma, unsigned long addr,
			  unsigned long *len)
{
	unsigned long off = addr - vma->vm_start;

	*len = 100 - off % 100;
	return (unsigned char *)vma->linuxu_backing + off;
}

static struct vm_area_struct *map(struct mm_struct *mm, unsigned long start,
				  unsigned long pages, unsigned long flags)
{
	struct vm_area_struct *vma = vm_area_alloc(mm);

	assert(vma && vma->vm_mm == mm);
	vma->vm_start = start;
	vma->vm_end = start + pages * PAGE_SIZE;
	vma->vm_flags = flags;
	return vma;
}

static void check_vma_registry_and_uaccess(struct linuxu_process *proc)
{
	struct linuxu_process_saved saved;
	struct mm_struct *mm = linuxu_process_mm(proc);
	struct vm_area_struct *rw, *ro, *gap, *bad;
	char buf[256];
	unsigned int value = 0xdeadbeef;
	void *pointer = (void *)1;

	assert(!linuxu_process_enter(proc, &saved));
	assert(current->mm == mm);

	rw = map(mm, UBASE, 2, VM_READ | VM_WRITE);
	rw->linuxu_kaddr = rw_backing;
	ro = map(mm, UBASE + 2 * PAGE_SIZE, 1, VM_READ);
	ro->linuxu_kmap = chunked_kmap;
	ro->linuxu_backing = ro_backing;
	/* A reservation with no backing two pages above. */
	gap = map(mm, UBASE + 5 * PAGE_SIZE, 1, VM_READ | VM_WRITE);
	mmap_write_lock(mm);
	assert(!linuxu_mm_insert_vma(mm, ro));
	assert(!linuxu_mm_insert_vma(mm, rw));
	assert(!linuxu_mm_insert_vma(mm, gap));
	bad = map(mm, UBASE + PAGE_SIZE, 2, VM_READ);	/* overlaps rw and ro */
	assert(linuxu_mm_insert_vma(mm, bad) == -EEXIST);
	bad->vm_start = UBASE + 3 * PAGE_SIZE + 1;	/* unaligned */
	assert(linuxu_mm_insert_vma(mm, bad) == -EINVAL);
	vm_area_free(bad);
	mmap_write_unlock(mm);
	assert(mm->map_count == 3 && mm->mmap == rw && rw->vm_next == ro && ro->vm_next == gap);

	/* Lookup and intersection, under mmap_lock as Linux requires. */
	mmap_read_lock(mm);
	assert(vma_lookup(mm, UBASE) == rw && vma_lookup(mm, UBASE + 2 * PAGE_SIZE - 1) == rw);
	assert(vma_lookup(mm, UBASE + 2 * PAGE_SIZE) == ro);
	assert(!vma_lookup(mm, UBASE - 1) && !vma_lookup(mm, UBASE + 3 * PAGE_SIZE));
	assert(find_vma(mm, 0) == rw && find_vma(mm, UBASE + 3 * PAGE_SIZE) == gap);
	assert(!find_vma(mm, UBASE + 6 * PAGE_SIZE));
	assert(!find_vma_intersection(mm, UBASE - PAGE_SIZE, UBASE));
	assert(find_vma_intersection(mm, UBASE - PAGE_SIZE, UBASE + 1) == rw);
	assert(find_vma_intersection(mm, UBASE + 3 * PAGE_SIZE, UBASE + 6 * PAGE_SIZE) == gap);
	assert(!find_vma_intersection(mm, UBASE + 3 * PAGE_SIZE, UBASE + 5 * PAGE_SIZE));
	assert(mmap_read_trylock(mm));	/* readers share */
	mmap_read_unlock(mm);
	mmap_read_unlock(mm);

	/* Hits. */
	assert(copy_to_user((void __user *)(UBASE + 5), "hello", 6) == 0);
	assert(!memcmp(rw_backing + 5, "hello", 6));
	memset(buf, 0, sizeof(buf));
	assert(copy_from_user(buf, (const void __user *)(UBASE + 5), 6) == 0);
	assert(!strcmp(buf, "hello"));
	assert(put_user(0x12345678u, (unsigned int __user *)(UBASE + 64)) == 0);
	assert(get_user(value, (const unsigned int __user *)(UBASE + 64)) == 0 &&
	       value == 0x12345678u);
	assert(clear_user((void __user *)(UBASE + 5), 3) == 0 && !rw_backing[5] && rw_backing[8] == 'l');
	/* A read spanning the rw/ro boundary and several kmap chunks. */
	for (unsigned int i = 0; i < 250; i++)
		ro_backing[i] = (unsigned char)i;
	memset(rw_backing + 2 * PAGE_SIZE - 6, 0xab, 6);
	assert(copy_from_user(buf, (const void __user *)(UBASE + 2 * PAGE_SIZE - 6), 256) == 0);
	for (unsigned int i = 0; i < 6; i++)
		assert((unsigned char)buf[i] == 0xab);
	for (unsigned int i = 6; i < 256; i++)
		assert((unsigned char)buf[i] == (unsigned char)(i - 6));
	strcpy((char *)rw_backing + 200, "kfd");
	assert(strncpy_from_user(buf, (const char __user *)(UBASE + 200), 16) == 3 && !strcmp(buf, "kfd"));
	assert(strnlen_user((const char __user *)(UBASE + 200), 16) == 4);

	/* Misses: partial copies report what is left and zero-fill reads. */
	memset(buf, 0x55, sizeof(buf));
	assert(copy_from_user(buf, (const void __user *)(UBASE + 3 * PAGE_SIZE - 4), 10) == 6);
	for (unsigned int i = 4; i < 10; i++)
		assert(!buf[i]);
	assert(copy_to_user((void __user *)(UBASE + 2 * PAGE_SIZE - 2), "abcd", 4) == 2);
	assert(rw_backing[2 * PAGE_SIZE - 1] == 'b');	/* stopped at the read-only VMA */
	assert(copy_to_user((void __user *)(UBASE + 5 * PAGE_SIZE), "x", 1) == 1);	/* no backing */

	/* EFAULT: no VMA at all, permission, address-space limit. */
	value = 7;
	assert(get_user(value, (const unsigned int __user *)0x1000) == -EFAULT && value == 0);
	assert(put_user(1u, (unsigned int __user *)0x1000) == -EFAULT);
	assert(put_user(1u, (unsigned int __user *)(UBASE + 2 * PAGE_SIZE)) == -EFAULT);
	assert(get_user(pointer, (void *const __user *)0x2000) == -EFAULT && !pointer);
	assert(strncpy_from_user(buf, (const char __user *)0x1000, 8) == -EFAULT);
	assert(!access_ok((void __user *)(TASK_SIZE_MAX - 2), 4));
	assert(copy_from_user(buf, (const void __user *)(TASK_SIZE_MAX - 2), 4) == 4);
	assert(access_ok((void __user *)UBASE, PAGE_SIZE));

	/* Unmap: the range faults again. */
	mmap_write_lock(mm);
	linuxu_mm_remove_vma(mm, ro);
	mmap_write_unlock(mm);
	assert(!vma_lookup(mm, UBASE + 2 * PAGE_SIZE) && mm->map_count == 2);
	assert(copy_from_user(buf, (const void __user *)(UBASE + 2 * PAGE_SIZE), 1) == 1);
	vm_area_free(ro);
	linuxu_process_leave(&saved);

	/* No mm outside the process: every user address faults. */
	assert(!current->mm);
	assert(copy_from_user(buf, (const void __user *)UBASE, 4) == 4);
	assert(get_user(value, (const unsigned int __user *)UBASE) == -EFAULT);
}

/* ---- PFN mappings: remap_pfn_range records what it would map ---- */
static void check_pfn_mapping(struct linuxu_process *proc)
{
	struct linuxu_process_saved saved;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned char buf[4];

	assert(!linuxu_process_enter(proc, &saved));
	mm = current->mm;
	vma = vm_area_alloc(mm);
	assert(vma);
	vma->vm_start = UBASE + 64 * PAGE_SIZE;
	vma->vm_end = vma->vm_start + 4 * PAGE_SIZE;
	vma->vm_flags = VM_READ | VM_WRITE | VM_SHARED;
	/* Outside the VMA, unaligned, or not starting at vm_start. */
	assert(remap_pfn_range(vma, vma->vm_start - PAGE_SIZE, 0x100, PAGE_SIZE, 0) == -EINVAL);
	assert(remap_pfn_range(vma, vma->vm_start, 0x100, 5 * PAGE_SIZE, 0) == -EINVAL);
	assert(remap_pfn_range(vma, vma->vm_start + 1, 0x100, PAGE_SIZE, 0) == -EINVAL);
	assert(remap_pfn_range(vma, vma->vm_start + PAGE_SIZE, 0x100, PAGE_SIZE, 0) == -EINVAL);
	/* A doorbell-style uncached mapping in two contiguous calls. */
	assert(!io_remap_pfn_range(vma, vma->vm_start, 0x100, 2 * PAGE_SIZE,
				   pgprot_noncached(0)));
	assert(remap_pfn_range(vma, vma->vm_start + 2 * PAGE_SIZE, 0x200, PAGE_SIZE,
			       pgprot_noncached(0)) == -EINVAL);	/* not contiguous */
	assert(!io_remap_pfn_range(vma, vma->vm_start + 2 * PAGE_SIZE, 0x102, 2 * PAGE_SIZE,
				   pgprot_noncached(0)));
	assert(vma->linuxu_pfn == 0x100 && vma->linuxu_pfn_bytes == 4 * PAGE_SIZE);
	assert(vma->linuxu_pfn_prot & LINUXU_PGPROT_NONCACHED);
	assert(!(pgprot_writecombine(vma->linuxu_pfn_prot) & LINUXU_PGPROT_NONCACHED));
	assert((vma->vm_flags & (VM_IO | VM_PFNMAP)) == (VM_IO | VM_PFNMAP));
	mmap_write_lock(mm);
	assert(!linuxu_mm_insert_vma(mm, vma));
	mmap_write_unlock(mm);
	/* No kernel backing: uaccess faults on an I/O mapping, as on Linux. */
	assert(copy_from_user(buf, (const void __user *)vma->vm_start, sizeof(buf)) == sizeof(buf));
	mmap_write_lock(mm);
	linuxu_mm_remove_vma(mm, vma);
	mmap_write_unlock(mm);
	vm_area_free(vma);
	linuxu_process_leave(&saved);
}

/* ---- signals ---- */
struct waiter {
	struct linuxu_process *proc;
	wait_queue_head_t wq;
	struct dma_fence *fence;
	int mode;		/* 0 waitqueue, 1 fence, 2 killable */
	volatile int entered;
	long result;
	bool fatal;
};

static void *waiter_main(void *arg)
{
	struct waiter *w = arg;
	struct linuxu_process_saved saved;
	int never = 0;

	assert(!linuxu_process_enter(w->proc, &saved));
	__atomic_store_n(&w->entered, 1, __ATOMIC_RELEASE);
	if (w->mode == 0)
		w->result = wait_event_interruptible(w->wq, never);
	else if (w->mode == 1)
		w->result = dma_fence_wait_timeout(w->fence, true, MAX_SCHEDULE_TIMEOUT);
	else
		w->result = wait_event_killable(w->wq, never);
	w->fatal = fatal_signal_pending(current);
	linuxu_process_leave(&saved);
	return NULL;
}

static void wait_entered(struct waiter *w)
{
	while (!__atomic_load_n(&w->entered, __ATOMIC_ACQUIRE))
		sched_yield();
	usleep(20000);	/* let it block */
}

static const char *fence_name(struct dma_fence *f) { (void)f; return "substrate"; }
static const struct dma_fence_ops fence_ops = {
	.get_driver_name = fence_name, .get_timeline_name = fence_name,
};

static void check_sigkill(void)
{
	struct linuxu_process *proc = linuxu_process_create(5150, "killme");
	struct linuxu_process *other = linuxu_process_create(5151, "bystander");
	struct linuxu_process_saved saved;
	struct waiter wq_waiter = { .proc = proc, .mode = 0 };
	struct waiter fence_waiter = { .proc = proc, .mode = 1 };
	struct dma_fence fence;
	pthread_t t1, t2;

	assert(proc && other);
	init_waitqueue_head(&wq_waiter.wq);
	dma_fence_init(&fence, &fence_ops, NULL, dma_fence_context_alloc(1), 1);
	fence_waiter.fence = &fence;
	assert(!pthread_create(&t1, NULL, waiter_main, &wq_waiter));
	assert(!pthread_create(&t2, NULL, waiter_main, &fence_waiter));
	wait_entered(&wq_waiter);
	wait_entered(&fence_waiter);
	assert(linuxu_process_active_threads(proc) == 2);

	linuxu_process_kill(proc);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	assert(wq_waiter.result == -ERESTARTSYS && wq_waiter.fatal);
	assert(fence_waiter.result == -ERESTARTSYS && fence_waiter.fatal);
	assert(!dma_fence_is_signaled(&fence));
	assert(linuxu_process_active_threads(proc) == 0);
	assert(fatal_signal_pending(linuxu_process_leader(proc)));

	/* A thread entering a killed group starts with SIGKILL pending; an
	 * uninterruptible fence wait still runs to its timeout. */
	assert(!linuxu_process_enter(proc, &saved));
	assert(fatal_signal_pending(current) && signal_pending(current));
	assert(dma_fence_wait_timeout(&fence, false, 2) == 0);
	assert(dma_fence_wait_timeout(&fence, true, 2) == -ERESTARTSYS);
	linuxu_process_leave(&saved);

	/* Other processes are unaffected. */
	assert(!linuxu_process_enter(other, &saved));
	assert(!signal_pending(current) && !fatal_signal_pending(current));
	assert(dma_fence_wait_timeout(&fence, true, 2) == 0);
	linuxu_process_leave(&saved);

	/* exit kills and drains a thread blocked in a killable wait. */
	struct waiter killable = { .proc = other, .mode = 2 };
	init_waitqueue_head(&killable.wq);
	assert(!pthread_create(&t1, NULL, waiter_main, &killable));
	wait_entered(&killable);
	linuxu_process_exit(other);
	pthread_join(t1, NULL);
	assert(killable.result == -ERESTARTSYS && killable.fatal);

	dma_fence_signal(&fence);	/* stack fence: keep its initial reference */
	linuxu_process_exit(proc);
}

/* ---- task swap, pids ---- */
static void check_task_swap(struct linuxu_process *p1, struct linuxu_process *p2)
{
	struct task_struct *base = current, *t1, *t2, *t3;
	struct linuxu_process_saved s1, s2, s3;
	struct task_struct *leader1 = linuxu_process_leader(p1);
	struct mm_struct *mm1 = linuxu_process_mm(p1);
	struct pid *pid;

	assert(leader1->pid == 4242 && leader1->tgid == 4242 && leader1->group_leader == leader1);
	assert(leader1->mm == mm1 && leader1->files == linuxu_process_files(p1));
	assert(!strcmp(leader1->comm, "proc-a"));

	assert(!linuxu_process_enter(p1, &s1));
	t1 = current;
	assert(t1 != base && t1 != leader1 && t1->group_leader == leader1);
	assert(t1->tgid == 4242 && t1->pid != 4242 && task_tgid_nr(t1) == 4242);
	assert(t1->mm == mm1 && t1->files == linuxu_process_files(p1));
	assert(atomic_read(&mm1->mm_users) == 2);
	pid = get_task_pid(current, PIDTYPE_TGID);
	assert(pid_vnr(pid) == 4242 && pid_task(pid, PIDTYPE_TGID) == leader1);
	put_pid(pid);
	pid = get_task_pid(current, PIDTYPE_PID);
	assert(pid_vnr(pid) == t1->pid);
	put_pid(pid);

	assert(!linuxu_process_enter(p2, &s2));
	t2 = current;
	assert(t2->group_leader == linuxu_process_leader(p2) && t2->tgid == 4343);
	assert(!linuxu_process_enter(p1, &s3));
	t3 = current;
	assert(t3 != t1 && t3->group_leader == leader1);
	assert(linuxu_process_active_threads(p1) == 2 && atomic_read(&mm1->mm_users) == 3);
	linuxu_process_leave(&s3);
	assert(current == t2);
	linuxu_process_leave(&s2);
	assert(current == t1);
	linuxu_process_leave(&s1);
	assert(current == base);
	assert(linuxu_process_active_threads(p1) == 0 && atomic_read(&mm1->mm_users) == 1);
}

/* ---- descriptor isolation ---- */
static int fd_a, fd_b;

static void check_fd_isolation(struct linuxu_process *p1, struct linuxu_process *p2)
{
	struct linuxu_process_saved saved;
	struct file *file;

	assert(!linuxu_process_enter(p1, &saved));
	fd_a = anon_inode_getfd("a", &fops, &file_tag_a, O_RDWR);
	assert(fd_a == 0);
	linuxu_process_leave(&saved);

	assert(!linuxu_process_enter(p2, &saved));
	assert(!fget(fd_a));	/* p1's descriptor is not visible */
	fd_b = anon_inode_getfd("b", &fops, &file_tag_b, O_RDWR);
	assert(fd_b == fd_a);	/* same number, own table */
	file = fget(fd_b);
	assert(file && file->private_data == &file_tag_b);
	fput(file);
	linuxu_process_leave(&saved);

	assert(!linuxu_process_enter(p1, &saved));
	file = fget(fd_a);
	assert(file && file->private_data == &file_tag_a);
	fput(file);
	linuxu_process_leave(&saved);

	/* The kernel table (no process) is a third, separate table. */
	assert(!fget(fd_a));
	assert(linuxu_files_count(linuxu_process_files(p1)) == 1);
	assert(linuxu_files_count(linuxu_process_files(p2)) == 1);

	/* close_fd acts on the caller's table only. */
	assert(!linuxu_process_enter(p2, &saved));
	assert(close_fd(fd_b) == 0 && close_fd(fd_b) == -EBADF);
	linuxu_process_leave(&saved);
	assert(event_index("file b") >= 0 && event_index("file a") < 0);
	assert(linuxu_files_count(linuxu_process_files(p1)) == 1);
	nevents = 0;
}

/* ---- notifiers ---- */
static void check_notifiers(struct linuxu_process *p1)
{
	struct linuxu_process_saved saved;
	struct mm_struct *mm = linuxu_process_mm(p1);
	struct mmu_notifier *a1, *a2, *b, *c;

	assert(!linuxu_process_enter(p1, &saved));
	a1 = mmu_notifier_get(&ops_a, current->mm);
	a2 = mmu_notifier_get(&ops_a, current->mm);
	assert(!IS_ERR(a1) && a1 == a2 && a1->users == 2 && allocs == 1);
	assert(a1->mm == mm && a1->ops == &ops_a);
	b = mmu_notifier_get(&ops_b, current->mm);
	assert(!IS_ERR(b) && b != a1 && allocs == 2);
	assert(PTR_ERR(mmu_notifier_get(&ops_fail, current->mm)) == -ESRCH);
	/* Each registered subscription pins the mm. */
	assert(atomic_read(&mm->mm_count) == 3);
	mmu_notifier_put(a2);
	assert(a1->users == 1 && !frees);

	/* The last put frees through SRCU without ->release. */
	c = mmu_notifier_get(&ops_c, current->mm);
	assert(!IS_ERR(c) && atomic_read(&mm->mm_count) == 4);
	mmu_notifier_put(c);
	mmu_notifier_synchronize();
	assert(frees == 1 && event_index("free c") >= 0 && event_index("release c") < 0);
	assert(atomic_read(&mm->mm_count) == 3);

	/* get_task_mm and kthread_use_mm. */
	struct mm_struct *got = get_task_mm(current->group_leader);
	assert(got == mm && atomic_read(&mm->mm_users) == 3);
	mmput(got);
	linuxu_process_leave(&saved);

	struct task_struct kthread = { .flags = PF_KTHREAD, .mm = mm };
	assert(!get_task_mm(&kthread));
	assert(!current->mm);
	got = get_task_mm(linuxu_process_leader(p1));
	kthread_use_mm(got);
	assert(current->mm == mm && atomic_read(&mm->mm_count) == 4);
	assert(copy_to_user((void __user *)(UBASE + 300), "w", 2) == 0);
	assert(!strcmp((char *)rw_backing + 300, "w"));
	kthread_unuse_mm(got);
	assert(!current->mm && atomic_read(&mm->mm_count) == 3);
	mmput(got);
	nevents = 0;
}

int main(void)
{
	struct linuxu_process *p1 = linuxu_process_create(4242, "proc-a");
	struct linuxu_process *p2 = linuxu_process_create(4343, "proc-b");

	assert(p1 && p2);
	check_task_swap(p1, p2);
	check_fd_isolation(p1, p2);
	check_vma_registry_and_uaccess(p1);
	check_pfn_mapping(p1);
	check_notifiers(p1);

	/* exit: ->release on every subscription (newest first, as the
	 * upstream hlist is), all before any descriptor closes. */
	struct mm_struct *mm = linuxu_process_mm(p1);
	struct task_struct *leader = get_task_struct(linuxu_process_leader(p1));
	mmgrab(mm);
	linuxu_process_exit(p1);
	assert(event_index("release b") == 0 && event_index("release a") == 1);
	assert(event_index("file a") > event_index("release a"));
	assert(!leader->mm && !leader->files && (leader->flags & PF_EXITING));
	assert(!get_task_mm(leader));
	/* a put itself from ->release; b still holds its subscription. */
	mmu_notifier_synchronize();
	assert(event_index("free a") >= 0 && event_index("free b") < 0);
	assert(atomic_read(&mm->mm_users) == 0 && atomic_read(&mm->mm_count) == 2);
	assert(!mm->mmap && !mm->map_count);	/* exit_mmap freed the VMAs */
	put_task_struct(leader);
	mmdrop(mm);

	linuxu_process_exit(p2);
	check_sigkill();
	puts("process substrate: task swap, fd isolation, notifier get/put and exit order, "
	     "VMA registry, uaccess hit/miss/EFAULT, PFN mappings, get_task_mm/kthread_use_mm and SIGKILL "
	     "wakeups passed");
	return 0;
}
