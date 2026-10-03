/* Upstream kfd_create_process on a linuxu process.
 *
 * Links the unmodified third_party/linux/drivers/gpu/drm/amd/amdkfd/kfd_process.c together with the real
 * kfd_events.c, kfd_process_queue_manager.c, kfd_flat_memory.c,
 * kfd_debug.c, kfd_smi_events.c, kfd_doorbell.c and kfd_debugfs.c, on the
 * linuxu process substrate (task/mm/files, mmu notifiers, SRCU,
 * workqueues). A thread task of a linuxu process opens a KFD process as a
 * /dev/kfd open does on Linux: kfd_create_process(current) passes the mm
 * check, registers the process's mmu notifier through mmu_notifier_get, and
 * is found again by mm from another thread of the same process.
 * linuxu_process_exit then drives the Linux exit path: the notifier
 * ->release starts KFD's teardown (exit_mm), the descriptor holding the
 * process is released (exit_files), and the deferred work frees it.
 *
 * Fixtures stand in for a probed GPU: KFD reports one GPU node, but the
 * topology lists no bound kfd_node, so kfd_init_apertures creates no
 * process-device data. Everything that needs a device (DQM, GPUVM, MES,
 * gfx-off) is a tripwire that fails the test if reached. No DriverKit
 * service, MMIO or hardware is used. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/workqueue.h>
#include <rt/process.h>
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"

/* ---- fixtures: a KFD instance with one GPU node and nothing bound ---- */
uint32_t kfd_gpu_node_num(void) { return 1; }
bool kfd_is_locked(struct kfd_dev *kfd) { (void)kfd; return false; }
int kfd_topology_enum_kfd_devices(uint8_t idx, struct kfd_node **kdev)
{
	*kdev = NULL;		/* index 0: a CPU node; nothing after it */
	return idx == 0 ? 0 : -1;
}
struct kfd_node *kfd_device_by_id(uint32_t gpu_id) { (void)gpu_id; return NULL; }
struct kfd_topology_device *kfd_topology_device_by_id(uint32_t gpu_id)
{
	(void)gpu_id;
	return NULL;
}

/* ---- tripwires: reachable only with a bound device ---- */
#define TRIPWIRE(name) do { \
	fprintf(stderr, "kfd_process test: device-only path %s reached\n", name); \
	abort(); \
} while (0)
int amdgpu_amdkfd_alloc_kernel_mem(struct amdgpu_device *a, size_t s, u32 d, void **m,
		uint64_t *g, void **c, bool q)
{ (void)a; (void)s; (void)d; (void)m; (void)g; (void)c; (void)q; TRIPWIRE("alloc_kernel_mem"); }
void amdgpu_amdkfd_free_kernel_mem(struct amdgpu_device *a, void **m)
{ (void)a; (void)m; TRIPWIRE("free_kernel_mem"); }
int amdgpu_amdkfd_gpuvm_free_memory_of_gpu(struct amdgpu_device *a, struct kgd_mem *m,
		void *d, uint64_t *s)
{ (void)a; (void)m; (void)d; (void)s; TRIPWIRE("gpuvm_free_memory_of_gpu"); }
int amdgpu_amdkfd_gpuvm_restore_process_bos(void *i, struct dma_fence __rcu **ef)
{ (void)i; (void)ef; TRIPWIRE("gpuvm_restore_process_bos"); }
void amdgpu_amdkfd_gpuvm_unmap_gtt_bo_from_kernel(struct kgd_mem *m)
{ (void)m; TRIPWIRE("gpuvm_unmap_gtt_bo_from_kernel"); }
int amdgpu_amdkfd_gpuvm_unmap_memory_from_gpu(struct amdgpu_device *a, struct kgd_mem *m, void *d)
{ (void)a; (void)m; (void)d; TRIPWIRE("gpuvm_unmap_memory_from_gpu"); }
int amdgpu_amdkfd_remove_gws_from_process(void *i, void *m)
{ (void)i; (void)m; TRIPWIRE("remove_gws_from_process"); }
int amdgpu_amdkfd_send_close_event_drain_irq(struct amdgpu_device *a, uint32_t *p)
{ (void)a; (void)p; TRIPWIRE("send_close_event_drain_irq"); }
void amdgpu_bo_free_kernel(struct amdgpu_bo **b, u64 *g, void **c)
{ (void)b; (void)g; (void)c; TRIPWIRE("bo_free_kernel"); }
void amdgpu_gfx_off_ctrl(struct amdgpu_device *a, bool e)
{ (void)a; (void)e; TRIPWIRE("gfx_off_ctrl"); }
int amdgpu_mes_flush_shader_debugger(struct amdgpu_device *a, uint64_t p, uint32_t x)
{ (void)a; (void)p; (void)x; TRIPWIRE("mes_flush_shader_debugger"); }
int amdgpu_mes_set_shader_debugger(struct amdgpu_device *a, uint64_t p, uint32_t s,
		const uint32_t *t, uint32_t f, bool trap_en, uint32_t x)
{ (void)a; (void)p; (void)s; (void)t; (void)f; (void)trap_en; (void)x; TRIPWIRE("mes_set_shader_debugger"); }
struct amdgpu_task_info *amdgpu_vm_get_task_info_vm(struct amdgpu_vm *vm)
{ (void)vm; TRIPWIRE("vm_get_task_info_vm"); }
void amdgpu_vm_put_task_info(struct amdgpu_task_info *t)
{ (void)t; TRIPWIRE("vm_put_task_info"); }
int debug_lock_and_unmap(struct device_queue_manager *d) { (void)d; TRIPWIRE("debug_lock_and_unmap"); }
int debug_map_and_unlock(struct device_queue_manager *d) { (void)d; TRIPWIRE("debug_map_and_unlock"); }
int debug_refresh_runlist(struct device_queue_manager *d) { (void)d; TRIPWIRE("debug_refresh_runlist"); }
bool kfd_dqm_is_queue_in_process(struct device_queue_manager *d, struct qcm_process_device *q,
		int o, u32 *f)
{ (void)d; (void)q; (void)o; (void)f; TRIPWIRE("dqm_is_queue_in_process"); }
int kfd_queue_release_buffers(struct kfd_process_device *p, struct queue_properties *q)
{ (void)p; (void)q; TRIPWIRE("queue_release_buffers"); }
int kfd_queue_unref_bo_vas(struct kfd_process_device *p, struct queue_properties *q)
{ (void)p; (void)q; TRIPWIRE("queue_unref_bo_vas"); }
int release_debug_trap_vmid(struct device_queue_manager *d, struct qcm_process_device *q)
{ (void)d; (void)q; TRIPWIRE("release_debug_trap_vmid"); }
int resume_queues(struct kfd_process *p, uint32_t n, uint32_t *ids)
{ (void)p; (void)n; (void)ids; TRIPWIRE("resume_queues"); }
void uninit_queue(struct queue *q) { (void)q; TRIPWIRE("uninit_queue"); }
bool amdgpu_sriov_xnack_support(struct amdgpu_device *a) { (void)a; TRIPWIRE("sriov_xnack_support"); }
/* kfd_debugfs.c's per-device show functions (read only on file access). */
int kfd_debugfs_hqds_by_device(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_hqds"); }
int kfd_debugfs_rls_by_device(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_rls"); }
int kfd_debugfs_hang_hws(struct kfd_node *dev) { (void)dev; TRIPWIRE("debugfs_hang_hws"); }
int kfd_debugfs_kfd_mem_limits(struct seq_file *m, void *d) { (void)m; (void)d; TRIPWIRE("debugfs_mem_limits"); }

/* ---- the /dev/kfd descriptor: release drops the open's reference, as
 * kfd_release (kfd_chardev.c) does ---- */
static unsigned int kfd_file_releases;
static int kfd_like_release(struct inode *inode, struct file *filep)
{
	(void)inode;
	kfd_file_releases++;
	kfd_unref_process(filep->private_data);
	return 0;
}
static const struct file_operations kfd_like_fops = { .release = kfd_like_release };

struct second_open {
	struct linuxu_process *proc;
	struct kfd_process *found;
};

/* Another OS thread of the same client: a second /dev/kfd open finds the
 * existing process by mm, as two threads of one Linux process do. */
static void *second_open_main(void *arg)
{
	struct second_open *s = arg;
	struct linuxu_process_saved saved;

	assert(!linuxu_process_enter(s->proc, &saved));
	s->found = kfd_create_process(current);
	assert(!IS_ERR(s->found) && s->found->lead_thread == current->group_leader);
	linuxu_process_leave(&saved);
	return NULL;
}

int main(void)
{
	struct linuxu_process *proc;
	struct linuxu_process_saved saved;
	struct kfd_process *p, *found;
	struct task_struct *leader;
	struct mm_struct *mm;
	pthread_t thread;
	int fd;

	/* The process half of kfd_init (kfd_module.c). procfs is left
	 * uncreated, a state kfd_init tolerates; the chardev and topology are
	 * the fixtures above. */
	assert(!kfd_process_create_wq());
	kfd_debugfs_init();

	/* A thread without an mm (a kernel thread on Linux) is still refused
	 * at the mm check. */
	assert(!current->mm);
	assert(PTR_ERR(kfd_create_process(current)) == -EINVAL);

	proc = linuxu_process_create(7001, "kfd-client");
	assert(proc);
	leader = get_task_struct(linuxu_process_leader(proc));
	mm = linuxu_process_mm(proc);
	mmgrab(mm);
	assert(atomic_read(&mm->mm_count) == 2);

	assert(!linuxu_process_enter(proc, &saved));
	p = kfd_create_process(current);
	assert(!IS_ERR(p));
	/* Upstream create_process: the KFD process belongs to this mm and
	 * thread group, and is primary. */
	assert(p->mm == current->mm && p->mm == mm);
	assert(p->lead_thread == leader && p->lead_thread == current->group_leader);
	assert(p->context_id == KFD_CONTEXT_ID_PRIMARY && p->n_pdds == 0);
	/* mmu_notifier_get registered p's notifier on the mm: it holds an
	 * mm_count pin and a process reference (with the open's, two). */
	assert(p->mmu_notifier.mm == mm && p->mmu_notifier.users == 1);
	assert(atomic_read(&mm->mm_count) == 3);
	assert(kref_read(&p->ref) == 2);
	/* create_process took a leader reference for lead_thread. */
	assert(refcount_read(&leader->usage) >= 3);
	found = kfd_lookup_process_by_mm(current->mm);
	assert(found == p);
	kfd_unref_process(found);
	/* The ioctl entry check (kfd_chardev.c): the caller's group leader. */
	assert(p->lead_thread == current->group_leader);

	/* The open's reference lives in a descriptor of this process. */
	fd = anon_inode_getfd("kfd", &kfd_like_fops, p, O_RDWR | O_CLOEXEC);
	assert(fd >= 0);
	linuxu_process_leave(&saved);

	struct second_open second = { .proc = proc };
	assert(!pthread_create(&thread, NULL, second_open_main, &second));
	pthread_join(thread, NULL);
	assert(second.found == p && kref_read(&p->ref) == 3);
	kfd_unref_process(second.found);	/* its descriptor's close */

	/* exit: mm release runs KFD's notifier release (table removal,
	 * p->mm = NULL, mmu_notifier_put), then the descriptor's release drops
	 * the open's reference and the release work frees the process. */
	linuxu_process_exit(proc);
	assert(kfd_file_releases == 1);
	assert(!kfd_lookup_process_by_mm(mm));
	/* kfd_exit order. */
	kfd_cleanup_processes();
	kfd_process_destroy_wq();	/* flushes kfd_process_wq_release */
	kfd_debugfs_fini();
	/* The notifier's mm pin and the lead_thread reference are gone: only
	 * the test's own references remain. */
	assert(atomic_read(&mm->mm_count) == 1 && atomic_read(&mm->mm_users) == 0);
	assert(refcount_read(&leader->usage) == 1);
	assert(!leader->mm && !leader->files);
	put_task_struct(leader);
	mmdrop(mm);
	puts("upstream kfd_create_process on a linuxu process: mm check, mmu_notifier_get, "
	     "lookup by mm across threads, exit_mm release, exit_files close and deferred free passed");
	return 0;
}
