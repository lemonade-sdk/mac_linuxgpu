/* linuxu: EDITED (third_party/linux/include/linux/kthread.h) - kthreads are
 * mapped onto GCD dispatch queues / pthreads by linuxu/src; struct
 * task_struct comes from linuxu linux/sched.h. API kept upstream. */
#ifndef _LINUX_KTHREAD_H
#define _LINUX_KTHREAD_H
/* Simple interface for creating and stopping kernel threads without mess. */
#include <linux/err.h>
#include <linux/sched.h>

struct mm_struct;

/*
 * When "(p->flags & PF_KTHREAD)" is set the task is a kthread and will
 * always remain a kthread.
 */
#define kthread_create(threadfn, data, namefmt, arg...) \
	kthread_create_on_node(threadfn, data, -1, namefmt, ##arg)

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char *namefmt,
					   ...) __printf(4, 5);

struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
					  void *data,
					  unsigned int cpu,
					  const char *namefmt);

void get_kthread_comm(char *buf, size_t buf_size, struct task_struct *tsk);
bool set_kthread_struct(struct task_struct *p);

void kthread_set_per_cpu(struct task_struct *k, int cpu)
	__attribute__((unavailable("DriverKit dispatch queues cannot bind to a CPU")));
bool kthread_is_per_cpu(struct task_struct *k);

/**
 * kthread_run - create and wake a thread.
 */
#define kthread_run(threadfn, data, namefmt, ...)			   \
({									   \
	struct task_struct *__k						   \
		= kthread_create(threadfn, data, namefmt, ## __VA_ARGS__); \
	if (!IS_ERR(__k))						   \
		wake_up_process(__k);					   \
	__k;								   \
})

/**
 * kthread_run_on_cpu - create and wake a cpu bound thread.
 */
static inline struct task_struct *
kthread_run_on_cpu(int (*threadfn)(void *data), void *data,
			unsigned int cpu, const char *namefmt)
{
	struct task_struct *p;

	p = kthread_create_on_cpu(threadfn, data, cpu, namefmt);
	if (!IS_ERR(p))
		wake_up_process(p);

	return p;
}

void kthread_bind(struct task_struct *k, unsigned int cpu)
	__attribute__((unavailable("DriverKit dispatch queues cannot bind to a CPU")));
int kthread_stop(struct task_struct *k);
int kthread_stop_put(struct task_struct *k);
bool kthread_should_stop(void);
bool kthread_should_park(void);
bool kthread_should_stop_or_park(void);
int kthread_park(struct task_struct *k);
void kthread_unpark(struct task_struct *k);
void kthread_parkme(void);

/* Host sleep does not implement the Linux task freezer protocol. */
#define KTW_FREEZABLE 1

struct kthread_worker {
	unsigned int flags;
	struct list_head work_list;
	struct task_struct *task;
	struct kthread_work *current_work;
	bool closing;
};
struct kthread_work {
	struct list_head node;
	void			(*func)(struct kthread_work *work);
	struct kthread_worker *worker;
	int canceling;
	unsigned long queue_seq;
};

void kthread_init_work(struct kthread_work *work,
		       void (*func)(struct kthread_work *work));

struct kthread_worker *kthread_create_worker_on_node(unsigned int flags,
	int node, const char *name_fmt, ...);
struct kthread_worker *kthread_create_worker(unsigned int flags,
	const char *name_fmt, ...);
struct kthread_worker *kthread_create_worker_on_cpu(int cpu, unsigned int flags,
	const char *name_fmt);
struct kthread_worker *kthread_run_worker(unsigned int flags,
	const char *name_fmt, ...);

void kthread_destroy_worker(struct kthread_worker *worker);

bool kthread_queue_work(struct kthread_worker *worker,
			 struct kthread_work *work);

void kthread_flush_work(struct kthread_work *work);

void kthread_flush_worker(struct kthread_worker *worker);

void kthread_flush_work_sync(struct kthread_worker *worker);

void kthread_cancel_work(struct kthread_worker *worker,
			 struct kthread_work *work);
bool kthread_cancel_work_sync(struct kthread_work *work);


#endif
