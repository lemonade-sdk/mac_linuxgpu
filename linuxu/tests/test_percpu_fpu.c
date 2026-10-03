/* Per-CPU variables are per task (linux/percpu.h): concurrent threads that
 * each run as "CPU 0" never share a this_cpu copy, a copy starts from the
 * definition's initial value, and the boot copy is untouched. Kernel FPU
 * sections (linux/fpu.h) are tracked per task, so DC's DC_FP_START/END
 * recursion depth (amdgpu_dm/dc_fpu.c) is per thread. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/fpu.h>
#include <linux/sched.h>
#include <rt/task.h>

#define ROUNDS 20000

static DEFINE_PER_CPU(int, depth) = 5;
static DEFINE_PER_CPU(long[4], arr);

static int arrivals;
/* Generation barrier for 5 parties (macOS has no pthread_barrier_t). */
static void barrier_wait(void)
{
	int gen = __atomic_add_fetch(&arrivals, 1, __ATOMIC_SEQ_CST);
	int target = (gen + 4) / 5 * 5;
	while (__atomic_load_n(&arrivals, __ATOMIC_SEQ_CST) < target)
		sched_yield();
}

/* The dc_fpu.c pattern: begin the FPU section only at depth 1. */
static void fp_start(void)
{
	if (this_cpu_inc_return(depth) == 6)
		kernel_fpu_begin();
}

static void fp_end(void)
{
	if (this_cpu_dec_return(depth) == 5)
		kernel_fpu_end();
}

static void *worker(void *arg)
{
	long id = (long)arg;

	assert(this_cpu_read(depth) == 5);	/* copy of the initial value */
	assert(!linuxu_kernel_fpu_active());
	barrier_wait();
	for (int i = 0; i < ROUNDS; i++) {
		fp_start();
		fp_start();
		assert(this_cpu_read(depth) == 7);
		assert(linuxu_kernel_fpu_active());
		fp_end();
		assert(linuxu_kernel_fpu_active());
		fp_end();
		assert(!linuxu_kernel_fpu_active());
		(*this_cpu_ptr(&arr))[id & 3] += 1;
	}
	assert(this_cpu_read(depth) == 5);
	assert((*this_cpu_ptr(&arr))[id & 3] == ROUNDS);
	assert((*this_cpu_ptr(&arr))[(id + 1) & 3] == 0);
	this_cpu_write(depth, 100 + (int)id);
	barrier_wait();
	assert(this_cpu_read(depth) == 100 + (int)id);
	linuxu_task_cleanup_current();
	return NULL;
}

int main(void)
{
	pthread_t threads[4];

	for (long i = 0; i < 4; i++)
		assert(pthread_create(&threads[i], NULL, worker, (void *)i) == 0);
	barrier_wait();
	/* The main task has its own copy; nothing it does disturbs workers. */
	kernel_fpu_begin();
	this_cpu_write(depth, -1);
	kernel_fpu_end();
	barrier_wait();
	for (int i = 0; i < 4; i++)
		assert(pthread_join(threads[i], NULL) == 0);
	assert(this_cpu_read(depth) == -1);
	assert(per_cpu(depth, 0) == 5);		/* boot copy unchanged */
	printf("per-task per-CPU copies and kernel FPU sections passed\n");
	return 0;
}
