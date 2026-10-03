#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/ww_mutex.h>
#include <linux/signal.h>
#include <rt/task.h>

static DECLARE_WAIT_QUEUE_HEAD(queue);
static atomic_int prepared;
static struct task_struct *retained;
static int callback_count;
static int free_entry(struct wait_queue_entry *entry, unsigned int mode, int sync, void *key)
{
	(void)mode; (void)sync; assert(key == NULL);
	callback_count++;
	list_del(&entry->entry);
	free(entry);
	return 1;
}
static void *waiter(void *unused)
{
	(void)unused;
	retained = get_task_struct(current);
	struct wait_queue_entry entry;
	init_wait(&entry);
	assert(!prepare_to_wait_event(&queue, &entry, TASK_INTERRUPTIBLE));
	atomic_store(&prepared, 1);
	assert(schedule_timeout(100) > 0);
	finish_wait(&queue, &entry);
	return NULL;
}
int main(void)
{
	alarm(15);
	for (int i = 0; i < 4; i++) {
		struct wait_queue_entry *entry = malloc(sizeof(*entry));
		init_waitqueue_func_entry(entry, free_entry);
		if (i < 2) add_wait_queue(&queue, entry);
		else add_wait_queue_exclusive(&queue, entry);
	}
	assert(wait_queue_active(&queue));
	assert(wake_up(&queue) == 3 && callback_count == 3);
	assert(wake_up_all(&queue) == 1 && callback_count == 4);
	assert(!waitqueue_active(&queue));
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, waiter, NULL));
	while (!atomic_load(&prepared)) usleep(100);
	assert(wake_up_interruptible(&queue) == 1);
	assert(!pthread_join(thread, NULL));
	assert(!waitqueue_active(&queue));
	assert(retained->group_leader == retained && retained->pid > 0);
	put_task_struct(retained);

	assert(!send_sig(SIGUSR1, current, 0));
	assert(wait_event_interruptible(queue, false) == -ERESTARTSYS);
	assert(wait_event_killable_timeout(queue, false, 1) == 0);
	assert(!send_sig(SIGKILL, current, 0));
	assert(wait_event_killable(queue, false) == -ERESTARTSYS);
	struct completion completion; init_completion(&completion);
	assert(wait_for_completion_interruptible(&completion) == -ERESTARTSYS);
	assert(wait_for_completion_killable_timeout(&completion, 10) == -ERESTARTSYS);
	struct mutex mutex; mutex_init(&mutex); mutex_lock(&mutex);
	assert(mutex_lock_interruptible(&mutex) == -EINTR);
	assert(mutex_lock_killable(&mutex) == -EINTR);
	mutex_unlock(&mutex);
	struct rw_semaphore sem; init_rwsem(&sem); down_write(&sem);
	assert(down_read_interruptible(&sem) == -EINTR);
	assert(down_write_killable(&sem) == -EINTR);
	assert(!atomic_long_read(&sem.waiters) && !atomic_long_read(&sem.writers_waiting));
	up_write(&sem);
	struct ww_mutex ww; DEFINE_WW_CLASS(ww_class); ww_mutex_init(&ww, &ww_class);
	assert(!ww_mutex_lock(&ww, NULL));
	assert(ww_mutex_lock_interruptible(&ww, NULL) == -EINTR);
	ww_mutex_unlock(&ww);
	unsigned long start = jiffies;
	assert(schedule_timeout_uninterruptible(2) == 0);
	assert(jiffies - start >= 2);
	linuxu_task_clear_signals(current);
	assert(!signal_pending(current));
	assert(wait_event_interruptible(queue, true) == 0);
	puts("waitqueue callbacks, exclusive wake, retained tasks, signals, interrupted locks and real uninterruptible deadlines passed");
}
