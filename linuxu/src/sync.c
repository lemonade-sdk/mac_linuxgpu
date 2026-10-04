/* linuxu shim: spinlock, mutex, rwlock, completion, and wait queue operations.
 * Lock and completion state lives in the Linux objects, so temporary objects
 * do not leave address-keyed entries behind after they are freed. */
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <rt/task.h>
#include <rt/park.h>
#include <rt/fatal.h>

/* ------------------------------------------------------------------ *
 * spinlock
 * ------------------------------------------------------------------ */

/* The lock word lives in the Linux object, so freeing/reusing a lock does
 * not leave a permanent address-keyed side table entry. IRQ callbacks run
 * on separate DriverKit queues; acquire/release orders shared CPU state. */
static void spin_pause(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("pause" ::: "memory");
#endif
}

void raw_spin_lock_init(raw_spinlock_t *lock)
{
	lock->owner = 0;
	lock->count = 1;
	lock->wait_lock = 0;
}

void spin_lock_init(spinlock_t *lock)
{
	raw_spin_lock_init(&lock->rlock);
}

void rwlock_init(rwlock_t *lock)
{
	rwsem_init(lock);
	raw_spin_lock_init(&lock->wait_lock);
	lock->first_waiter = NULL;
}

void seqcount_init(struct seqcount *s)
{
	s->seqcount = 0;
}

/* Process-wide count of held spinlocks, for linuxu_fatal() diagnostics
 * only: a thread parked while holding one leaves its waiters spinning.
 * Relaxed and approximate (re-initializing a held lock is not tracked). */
static long spinlocks_held;

long linuxu_spinlocks_held(void)
{
	return __atomic_load_n(&spinlocks_held, __ATOMIC_RELAXED);
}

void spin_lock(spinlock_t *lock)
{
	while (__atomic_exchange_n(&lock->rlock.wait_lock, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&lock->rlock.wait_lock, __ATOMIC_RELAXED))
			spin_pause();
	__atomic_store_n(&lock->rlock.owner,
		(unsigned int)(uintptr_t)pthread_self(), __ATOMIC_RELAXED);
	__atomic_add_fetch(&spinlocks_held, 1, __ATOMIC_RELAXED);
}

void spin_unlock(spinlock_t *lock)
{
	__atomic_sub_fetch(&spinlocks_held, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&lock->rlock.owner, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&lock->rlock.wait_lock, 0, __ATOMIC_RELEASE);
}

int spin_trylock(spinlock_t *lock)
{
	unsigned int expected = 0;
	if (!__atomic_compare_exchange_n(&lock->rlock.wait_lock, &expected, 1,
		false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return 0;
	__atomic_store_n(&lock->rlock.owner,
		(unsigned int)(uintptr_t)pthread_self(), __ATOMIC_RELAXED);
	__atomic_add_fetch(&spinlocks_held, 1, __ATOMIC_RELAXED);
	return 1;
}

int spin_trylock_irq(spinlock_t *lock)
{
	return spin_trylock(lock);
}

void spin_lock_irq(spinlock_t *lock)
{
	spin_lock(lock);
}

void spin_unlock_irq(spinlock_t *lock)
{
	spin_unlock(lock);
}

void (spin_lock_irqsave)(spinlock_t *lock, irqflags_t flags)
{
	(void)flags;
	spin_lock(lock);
}

void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags)
{
	(void)flags;
	spin_unlock(lock);
}

bool spin_is_locked(spinlock_t *lock)
{
	return __atomic_load_n(&lock->rlock.wait_lock, __ATOMIC_RELAXED) != 0;
}

void spin_lock_nested(spinlock_t *lock, int subclass)
{
	(void)subclass;
	spin_lock(lock);
}

bool spin_trylock_nested(spinlock_t *lock, int subclass)
{
	(void)subclass;
	return spin_trylock(lock);
}

void spin_lock_bh(spinlock_t *lock) { spin_lock(lock); }
void spin_unlock_bh(spinlock_t *lock) { spin_unlock(lock); }
bool spin_trylock_bh(spinlock_t *lock) { return spin_trylock(lock); }
void spin_lock_irqsave_nobh(spinlock_t *lock, irqflags_t *flags)
{ if (flags) *flags = 0; (spin_lock_irqsave)(lock, 0); }
void spin_unlock_irqrestore_nobh(spinlock_t *lock, irqflags_t flags)
{ spin_unlock_irqrestore(lock, flags); }

/* ---- raw_spinlock aliases ---- */
void raw_spin_lock(raw_spinlock_t *lock)
{ spin_lock((spinlock_t *)lock); }
void raw_spin_unlock(raw_spinlock_t *lock)
{ spin_unlock((spinlock_t *)lock); }
int raw_spin_trylock(raw_spinlock_t *lock)
{ return spin_trylock((spinlock_t *)lock); }
void raw_spin_lock_irq(raw_spinlock_t *lock)
{ spin_lock_irq((spinlock_t *)lock); }
void raw_spin_unlock_irq(raw_spinlock_t *lock)
{ spin_unlock_irq((spinlock_t *)lock); }
void (raw_spin_lock_irqsave)(raw_spinlock_t *lock, irqflags_t flags)
{ (spin_lock_irqsave)((spinlock_t *)lock, flags); }
void raw_spin_unlock_irqrestore(raw_spinlock_t *lock, irqflags_t flags)
{ spin_unlock_irqrestore((spinlock_t *)lock, flags); }
bool raw_spin_is_locked(const raw_spinlock_t *lock)
{ return spin_is_locked((spinlock_t *)(uintptr_t)lock); }

/* ---- rwlock_t: unified on struct rw_semaphore (see rwsem ops) ---- */
void rwlock_read_lock(rwlock_t *lock) { down_read(lock); }
void rwlock_read_unlock(rwlock_t *lock) { up_read(lock); }
int rwlock_read_trylock(rwlock_t *lock) { return down_read_trylock(lock) ? 1 : 0; }
void rwlock_write_lock(rwlock_t *lock) { down_write(lock); }
void rwlock_write_unlock(rwlock_t *lock) { up_write(lock); }
int rwlock_write_trylock(rwlock_t *lock) { return down_write_trylock(lock) ? 1 : 0; }
void rwlock_read_lock_irqsave(rwlock_t *lock, irqflags_t flags)
{ (void)flags; down_read(lock); }
void rwlock_read_unlock_irqrestore(rwlock_t *lock, irqflags_t flags)
{ (void)flags; up_read(lock); }
void rwlock_write_lock_irqsave(rwlock_t *lock, irqflags_t *flags)
{ if (flags) *flags = 0; down_write(lock); }
void rwlock_write_unlock_irqrestore(rwlock_t *lock, irqflags_t flags)
{ (void)flags; up_write(lock); }

void seqcount_lock_begin(struct seqcount *s)
{
	__atomic_add_fetch(&s->seqcount, 1, __ATOMIC_RELAXED);
	/* Publish the odd counter before any protected data changes. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}
void seqcount_unlock(struct seqcount *s)
{
	__atomic_add_fetch(&s->seqcount, 1, __ATOMIC_RELEASE);
}
unsigned int seqcount_raw_read_begin(const struct seqcount *s)
{
	unsigned int sequence;
	do {
		sequence = __atomic_load_n(&s->seqcount, __ATOMIC_ACQUIRE);
		if (sequence & 1) spin_pause();
	} while (sequence & 1);
	return sequence;
}
bool seqcount_retry(const struct seqcount *s, unsigned int sequence)
{
	/* Data reads must finish before checking the writer's final counter. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&s->seqcount, __ATOMIC_RELAXED) != sequence;
}

/* ------------------------------------------------------------------ *
 * mutex
 * ------------------------------------------------------------------ */

/* The existing wait_lock word belongs to the mutex object and starts at zero
 * for both DEFINE_MUTEX and zeroed dynamic objects. Sleep between attempts
 * so a contended mutex does not spin for an entire long critical section. */

void __mutex_init_generic(struct mutex *lock)
{
	atomic_long_set(&lock->owner, 0);
	raw_spin_lock_init(&lock->wait_lock);
	lock->first_waiter = NULL;
}

void mutex_destroy(struct mutex *lock)
{
	/* Callers must have released the mutex before destroying it. */
	if (lock && !raw_spin_is_locked(&lock->wait_lock))
		__mutex_init_generic(lock);
}

bool mutex_is_locked(struct mutex *lock)
{
	return lock && raw_spin_is_locked(&lock->wait_lock);
}

/* Contended: park on the mutex until an unlock (rt/park.h), with the
 * backstop for an unlock that did not wake. @mode 0 uninterruptible, 1
 * interruptible, 2 killable. */
static bool mutex_still_locked(const void *p)
{
	return raw_spin_is_locked(&((const struct mutex *)p)->wait_lock);
}

static int mutex_wait(struct mutex *lock, int mode, const void *caller)
{
	uint64_t backstop = linuxu_park_backstop_first();
	bool backstopped = false;
	int r = 0;

	for (;;) {
		__atomic_add_fetch(&lock->linuxu_waiters, 1, __ATOMIC_SEQ_CST);
		if (raw_spin_trylock(&lock->wait_lock)) {
			if (backstopped)
				linuxu_wait_missed(NULL, caller);
			break;
		}
		if (mode && linuxu_wait_signal_pending(mode == 2)) {
			r = -EINTR;
			break;
		}
		if (linuxu_park(lock, mutex_still_locked, lock,
				linuxu_park_now_ns() + backstop) == LINUXU_PARK_WOKEN) {
			backstopped = false;
			backstop = linuxu_park_backstop_first();
		} else {
			backstopped = true;
			backstop = linuxu_park_backstop_next(backstop);
		}
		__atomic_sub_fetch(&lock->linuxu_waiters, 1, __ATOMIC_SEQ_CST);
	}
	__atomic_sub_fetch(&lock->linuxu_waiters, 1, __ATOMIC_SEQ_CST);
	if (!r)
		atomic_long_set(&lock->owner, (long)(uintptr_t)pthread_self());
	return r;
}

void mutex_lock(struct mutex *lock)
{
	if (raw_spin_trylock(&lock->wait_lock))
		atomic_long_set(&lock->owner, (long)(uintptr_t)pthread_self());
	else
		(void)mutex_wait(lock, 0, __builtin_return_address(0));
}

int mutex_lock_interruptible(struct mutex *lock)
{
	if (mutex_trylock(lock))
		return 0;
	return mutex_wait(lock, 1, __builtin_return_address(0));
}

int mutex_lock_killable(struct mutex *lock)
{
	if (mutex_trylock(lock))
		return 0;
	return mutex_wait(lock, 2, __builtin_return_address(0));
}

void mutex_lock_io(struct mutex *lock) { mutex_lock(lock); }

void mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	(void)subclass;
	mutex_lock(lock);
}

int mutex_lock_interruptible_nested(struct mutex *lock,
				    unsigned int subclass)
{
	return mutex_lock_interruptible(lock);
}

int mutex_lock_killable_nested(struct mutex *lock,
			       unsigned int subclass)
{
	return mutex_lock_killable(lock);
}

void mutex_unlock(struct mutex *lock)
{
	atomic_long_set(&lock->owner, 0);
	raw_spin_unlock(&lock->wait_lock);
	/* Pairs with mutex_wait's increment before its trylock. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (__atomic_load_n(&lock->linuxu_waiters, __ATOMIC_SEQ_CST))
		linuxu_unpark(lock);
}

int mutex_trylock(struct mutex *lock)
{
	int r = raw_spin_trylock(&lock->wait_lock);
	if (r)
		atomic_long_set(&lock->owner, (long)(uintptr_t)pthread_self());
	return r;
}

/* ------------------------------------------------------------------ *
 * completion
 *
 * The done counter and its lock live in each completion object. This keeps
 * static, stack, and temporary completions independent without a destructor.
 * ------------------------------------------------------------------ */

void init_completion(struct completion *x)
{
	raw_spin_lock_init(&x->lock);
	x->done = 0;
	INIT_LIST_HEAD(&x->wait);
}

void init_completion_done(struct completion *x)
{
	init_completion(x);
	x->done = 1;
}

void reinit_completion(struct completion *x)
{
	raw_spin_lock(&x->lock);
	x->done = 0;
	raw_spin_unlock(&x->lock);
}

void complete(struct completion *x)
{
	raw_spin_lock(&x->lock);
	if (x->done < UINT_MAX)
		x->done++;
	raw_spin_unlock(&x->lock);
	linuxu_unpark(x);
}

void complete_all(struct completion *x)
{
	raw_spin_lock(&x->lock);
	x->done = UINT_MAX;
	raw_spin_unlock(&x->lock);
	linuxu_unpark(x);
}

void complete_done(struct completion *x)
{
	complete(x);
}

int completion_done(struct completion *x)
{
	unsigned int d;
	raw_spin_lock(&x->lock);
	d = x->done;
	raw_spin_unlock(&x->lock);
	return d;
}

static uint64_t completion_now_ns(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_UPTIME_RAW, &now))
		LINUXU_FATAL("completion: uptime clock unavailable");
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool completion_still(const void *p)
{
	return !__atomic_load_n(&((const struct completion *)p)->done, __ATOMIC_SEQ_CST);
}

/* Parks on the completion (complete() unparks it), with the backstop for a
 * completion signalled without a wake and for a signal to an interruptible
 * waiter (signals wake tasks, not completions). */
static long comp_wait(const void *caller, struct completion *x,
			       unsigned long timeout_jiffies, bool timed, int interruptible)
{
	const uint64_t tick_ns = 1000000000ULL / HZ;
	uint64_t deadline = 0, backstop = linuxu_park_backstop_first();
	bool backstopped = false;

	if (timed) {
		uint64_t now = completion_now_ns();
		deadline = timeout_jiffies > (UINT64_MAX - now) / tick_ns ?
			UINT64_MAX : now + (uint64_t)timeout_jiffies * tick_ns;
	}
	for (;;) {
		uint64_t now, remaining, until;
		unsigned int done;

		raw_spin_lock(&x->lock);
		done = x->done;
		if (done && done != UINT_MAX)
			x->done = done - 1;
		raw_spin_unlock(&x->lock);
		if (done) {
			if (backstopped)
				linuxu_wait_missed(NULL, caller);
			if (!timed)
				return 1;
			now = completion_now_ns();
			remaining = now < deadline ? deadline - now : 0;
			/* Linux reports at least one jiffy on a successful wait. */
			return remaining ? (unsigned long)((remaining - 1) / tick_ns + 1) : 1;
		}
		now = completion_now_ns();
		if (timed && now >= deadline)
			return 0;
		if (interruptible && linuxu_wait_signal_pending(interruptible == 2))
			return -ERESTARTSYS;
		until = now + backstop;
		if (timed && deadline < until)
			until = deadline;
		if (linuxu_park(x, completion_still, x, until) == LINUXU_PARK_WOKEN) {
			backstopped = false;
			backstop = linuxu_park_backstop_first();
		} else if (until != deadline) {
			backstopped = true;
			backstop = linuxu_park_backstop_next(backstop);
		}
	}
}


int wait_for_completion(struct completion *x)
{
	comp_wait(__builtin_return_address(0), x, 0, false, 0);
	return 0;
}

unsigned long wait_for_completion_timeout(struct completion *x,
					  unsigned long timeout)
{
	return comp_wait(__builtin_return_address(0), x, timeout, true, 0);
}

int wait_for_completion_killable(struct completion *x)
{
	long ret = comp_wait(__builtin_return_address(0), x, 0, false, 2);
	return ret < 0 ? (int)ret : 0;
}

long wait_for_completion_killable_timeout(struct completion *x,
						    unsigned long timeout)
{
	return comp_wait(__builtin_return_address(0), x, timeout, true, 2);
}

int wait_for_completion_interruptible(struct completion *x)
{
	long ret = comp_wait(__builtin_return_address(0), x, 0, false, 1);
	return ret < 0 ? (int)ret : 0;
}

long wait_for_completion_interruptible_timeout(struct completion *x,
							unsigned long timeout)
{
	return comp_wait(__builtin_return_address(0), x, timeout, true, 1);
}

/*
 * TODO(linuxu): wake-function variants need the wait-queue entry type;
 * the KMD's completion usage never passes a custom wake function, so
 * these run the default path.
 */
bool wait_for_completion_interruptible_wake_function(struct completion *x,
						     wake_function_t wake_function)
{
	(void)wake_function;
	return comp_wait(__builtin_return_address(0), x, 0, false, 1) > 0;
}

bool wait_for_completion_wake_function(struct completion *x,
				       wake_function_t wake_function)
{
	(void)wake_function;
	comp_wait(__builtin_return_address(0), x, 0, false, 0);
	return true;
}

void rearm_completion(struct completion *x)
{
	reinit_completion(x);
}

bool try_wait_for_completion(struct completion *x)
{
	bool was;
	raw_spin_lock(&x->lock);
	was = x->done != 0;
	if (was && x->done != UINT_MAX)
		x->done--;
	raw_spin_unlock(&x->lock);
	return was;
}

void complete_and_exit(struct completion *x, unsigned long v)
{
	(void)v;
	complete(x);
	/* in-process: just return; kthread_stop observes via its join */
}

/* ------------------------------------------------------------------ *
 * wait queues
 * ------------------------------------------------------------------ */

void init_waitqueue_head(struct wait_queue_head *wq_head)
{
	spin_lock_init(&wq_head->lock);
	INIT_LIST_HEAD(&wq_head->head);
}

struct wait_queue_head *alloc_wait_queue_head(gfp_t flags)
{
	struct wait_queue_head *wq = calloc(1, sizeof(*wq));

	(void)flags;
	if (wq)
		init_waitqueue_head(wq);
	return wq;
}

struct wait_queue_head *alloc_wait_queue_head_node(gfp_t flags, int node)
{
	(void)node;
	return alloc_wait_queue_head(flags);
}

void destroy_wait_queue_head(struct wait_queue_head *wq_head)
{
	free(wq_head);
}

void __add_wait_queue(struct wait_queue_head *wq_head,
		      struct wait_queue_entry *wq_entry)
{
	list_add(&wq_entry->entry, &wq_head->head);
}

void __add_wait_queue_exclusive(struct wait_queue_head *wq_head,
				struct wait_queue_entry *wq_entry)
{
	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	list_add_tail(&wq_entry->entry, &wq_head->head);
}

void __remove_wait_queue(struct wait_queue_head *wq_head,
			 struct wait_queue_entry *wq_entry)
{
	(void)wq_head;
	list_del_init(&wq_entry->entry);
}

int wait_queue_active(struct wait_queue_head *wq_head)
{
	return waitqueue_active(wq_head);
}

void prepare_to_wait(struct wait_queue_head *wq_head,
		     struct wait_queue_entry *wq_entry, int state)
{
	spin_lock(&wq_head->lock);
	if (list_empty(&wq_entry->entry))
		__add_wait_queue(wq_head, wq_entry);
	__set_current_state(state);
	spin_unlock(&wq_head->lock);
}

long prepare_to_wait_event(struct wait_queue_head *wq_head,
			   struct wait_queue_entry *wq_entry, int state)
{
	prepare_to_wait(wq_head, wq_entry, state);
	if ((state == TASK_INTERRUPTIBLE && linuxu_wait_signal_pending(false)) ||
	    (state == TASK_KILLABLE && linuxu_wait_signal_pending(true))) {
		finish_wait(wq_head, wq_entry);
		return -ERESTARTSYS;
	}
	return 0;
}

void finish_wait(struct wait_queue_head *wq_head,
		 struct wait_queue_entry *wq_entry)
{
	__set_current_state(TASK_RUNNING);
	spin_lock(&wq_head->lock);
	if (!list_empty(&wq_entry->entry)) list_del_init(&wq_entry->entry);
	spin_unlock(&wq_head->lock);
}

/* ---- wait_event (linux/wait.h): park on the task, the wake queue wakes it ---- */

void linuxu_waiter_init(struct linuxu_waiter *w, struct wait_queue_head *wq, int state,
			long timeout, bool timed, bool wq_locked, struct linuxu_wait_site *site)
{
	const uint64_t tick_ns = 1000000000ULL / HZ;

	memset(w, 0, sizeof(*w));
	w->wq = wq;
	w->task = current;
	w->site = site;
	w->state = state;
	w->timed = timed;
	w->wq_locked = wq_locked;
	w->backstop_ns = linuxu_park_backstop_first();
	init_waitqueue_entry(&w->entry, w->task);
	w->entry.func = autoremove_wake_function;
	if (timed && timeout < MAX_SCHEDULE_TIMEOUT) {
		const uint64_t now = linuxu_park_now_ns();
		const uint64_t left = timeout > 0 ? (uint64_t)timeout : 0;

		w->deadline_ns = left > (UINT64_MAX - now) / tick_ns ? UINT64_MAX :
			now + left * tick_ns;
		w->expired = !left;
	}
	/* A wake queue nobody initialized (zeroed memory): Linux code always
	 * initializes one; a shim may not have. */
	if (!wq->head.next) {
		if (!wq_locked)
			spin_lock(&wq->lock);
		if (!wq->head.next)
			INIT_LIST_HEAD(&wq->head);
		if (!wq_locked)
			spin_unlock(&wq->lock);
	}
}

void linuxu_waiter_prepare(struct linuxu_waiter *w)
{
	if (!w->wq_locked)
		spin_lock(&w->wq->lock);
	if (list_empty(&w->entry.entry))
		__add_wait_queue(w->wq, &w->entry);
	__set_current_state(w->state);
	if (!w->wq_locked)
		spin_unlock(&w->wq->lock);
	/* Before the caller tests its condition: a wake after this moves the
	 * sequence, so the sleep below returns at once. */
	w->seq = __atomic_load_n(&w->task->wake_sequence, __ATOMIC_SEQ_CST);
}

void linuxu_waiter_satisfied(struct linuxu_waiter *w)
{
	if (w->backstopped)
		linuxu_wait_missed(w->site, NULL);
}

int linuxu_waiter_sleep(struct linuxu_waiter *w)
{
	uint64_t now, until;

	if (w->expired)
		return 1;
	now = linuxu_park_now_ns();
	if (w->deadline_ns && now >= w->deadline_ns)
		return 1;
	until = now + w->backstop_ns;
	if (w->deadline_ns && w->deadline_ns < until)
		until = w->deadline_ns;
	if (linuxu_park_task(w->task, w->seq, until) == LINUXU_PARK_WOKEN) {
		w->backstopped = false;
		w->backstop_ns = linuxu_park_backstop_first();
		return 0;
	}
	if (w->deadline_ns && linuxu_park_now_ns() >= w->deadline_ns)
		return 1;
	w->backstopped = true;
	w->backstop_ns = linuxu_park_backstop_next(w->backstop_ns);
	return 0;
}

long linuxu_waiter_left(struct linuxu_waiter *w)
{
	const uint64_t tick_ns = 1000000000ULL / HZ;
	uint64_t now;

	if (!w->deadline_ns && !w->expired)
		return MAX_SCHEDULE_TIMEOUT;
	now = linuxu_park_now_ns();
	if (w->expired || now >= w->deadline_ns)
		return 1;	/* Linux: at least one jiffy when the condition held */
	return (long)((w->deadline_ns - now + tick_ns - 1) / tick_ns);
}

void linuxu_waiter_finish(struct linuxu_waiter *w)
{
	__set_current_state(TASK_RUNNING);
	if (!w->wq_locked)
		spin_lock(&w->wq->lock);
	if (!list_empty(&w->entry.entry))
		list_del_init(&w->entry.entry);
	if (!w->wq_locked)
		spin_unlock(&w->wq->lock);
}

bool linuxu_wait_signal_pending(bool fatal_only)
{
	return fatal_only ? linuxu_task_fatal_signal_pending(current) : signal_pending(current);
}
int default_wake_function(struct wait_queue_entry *entry, unsigned int mode,
	int sync, void *key)
{
	(void)sync; (void)key;
	struct task_struct *task = entry->private;
	if (!task || !(__atomic_load_n(&task->state, __ATOMIC_ACQUIRE) & mode)) return 0;
	wake_up_process(task);
	return 1;
}
int autoremove_wake_function(struct wait_queue_entry *entry, unsigned int mode,
	int sync, void *key)
{
	int woke = default_wake_function(entry, mode, sync, key);
	if (woke) list_del_init(&entry->entry);
	return woke;
}
static unsigned int wake_locked(struct wait_queue_head *queue, unsigned int mode,
	unsigned int exclusive, int sync, void *key)
{
	unsigned int count = 0;
	struct list_head *node = queue->head.next;
	while (node != &queue->head) {
		struct wait_queue_entry *entry = list_entry(node, struct wait_queue_entry, entry);
		node = node->next;
		unsigned long flags = entry->flags;
		int result = entry->func ? entry->func(entry, mode, sync, key) : 0;
		if (result < 0) break;
		if (result > 0) {
			count++;
			if ((flags & WQ_FLAG_EXCLUSIVE) && exclusive && !--exclusive) break;
		}
	}
	return count;
}
static unsigned int wake_queue(struct wait_queue_head *queue, unsigned int mode,
	unsigned int exclusive, int sync, void *key)
{
	spin_lock(&queue->lock);
	unsigned int result = wake_locked(queue, mode, exclusive, sync, key);
	spin_unlock(&queue->lock);
	return result;
}

unsigned int wake_up_interruptible(struct wait_queue_head *wq_head)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE, 1, 0, NULL);
}

unsigned int wake_up_interruptible_sync(struct wait_queue_head *wq_head)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE, 1, 1, NULL);
}

unsigned int wake_up_interruptible_poll(struct wait_queue_head *wq_head,
					unsigned int key)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE, 1, 0, (void *)(uintptr_t)key);
}

unsigned int wake_up(struct wait_queue_head *wq_head)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_KILLABLE, 1, 0, NULL);
}

unsigned int wake_up_all(struct wait_queue_head *wq_head)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_KILLABLE, 0, 0, NULL);
}

unsigned int wake_up_interruptible_nr(struct wait_queue_head *wq_head,
				      unsigned int nr)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE, nr, 0, NULL);
}

unsigned int wake_up_all_nr(struct wait_queue_head *wq_head, unsigned int nr)
{
	return wake_queue(wq_head, TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_KILLABLE, nr, 0, NULL);
}

unsigned int wake_up_all_locked(struct wait_queue_head *queue)
{
	return wake_locked(queue, TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_KILLABLE, 0, 0, NULL);
}



/* Sleep until the task is woken (linuxu_task_wake: wake queues,
 * wake_up_process, signals) or the timeout, parked (rt/park.h). Returns
 * the remaining Linux jiffies. As in Linux the timeout ends at a jiffy
 * boundary (the tick @timeout jiffies after the current one), and a task
 * still TASK_RUNNING (a poll, or a wake that came before the sleep) does
 * not sleep at all. A wait longer than the backstop maximum returns early
 * with time left, a spurious wake as Linux allows: callers re-check their
 * condition and sleep again. */
long schedule_timeout(long timeout)
{
	const uint64_t tick_ns = 1000000000ULL / HZ;
	struct task_struct *task = current;
	uint64_t now, expire = 0, deadline = 0;

	if (timeout <= 0)
		return 0;
	now = linuxu_park_now_ns();
	if (timeout != MAX_SCHEDULE_TIMEOUT) {
		expire = now / tick_ns + (uint64_t)timeout;
		deadline = expire > UINT64_MAX / tick_ns ? UINT64_MAX : expire * tick_ns;
	}
	for (;;) {
		const unsigned long seq = __atomic_load_n(&task->wake_sequence, __ATOMIC_SEQ_CST);
		const long state = __atomic_load_n(&task->state, __ATOMIC_ACQUIRE);
		uint64_t until;

		if (state == TASK_RUNNING)
			break;
		if ((state == TASK_INTERRUPTIBLE && signal_pending(task)) ||
		    (state == TASK_KILLABLE && linuxu_task_fatal_signal_pending(task)))
			break;
		now = linuxu_park_now_ns();
		if (deadline && now >= deadline)
			break;
		until = now + linuxu_park_backstop_max();
		if (deadline && deadline < until)
			until = deadline;
		if (linuxu_park_task(task, seq, until) == LINUXU_PARK_TIMEOUT && until != deadline)
			break;	/* the backstop: a spurious wake */
	}
	__atomic_store_n(&task->state, TASK_RUNNING, __ATOMIC_RELEASE);
	if (timeout == MAX_SCHEDULE_TIMEOUT)
		return timeout;
	now = linuxu_park_now_ns() / tick_ns;
	return now >= expire ? 0 : (long)(expire - now);
}

long schedule_timeout_interruptible(long timeout)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(timeout);
}

long schedule_timeout_killable(long timeout)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(timeout);
}

void schedule(void)
{
	(void)schedule_timeout(MAX_SCHEDULE_TIMEOUT);
}

int schedule_tail(void)
{
	return 0;
}

/* A sync file owns the fence until the last file reference is released. */
#include <linux/sync_file.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/fs.h>

static int sync_file_release(struct inode *inode, struct file *file)
{
	(void)inode;
	struct sync_file *sf = file->private_data;
	dma_fence_put(sf->fence);
	kfree(sf);
	return 0;
}

static const struct file_operations sync_file_operations = {
	.release = sync_file_release,
};

struct sync_file *sync_file_create(struct dma_fence *fence)
{
	if (!fence)
		return NULL;
	struct sync_file *sf = kzalloc(sizeof(*sf), GFP_KERNEL);

	if (!sf)
		return NULL;
	init_waitqueue_head(&sf->wq);
	sf->fence = dma_fence_get(fence);
	sf->file = anon_inode_getfile("sync_file", &sync_file_operations, sf, O_RDWR);
	if (IS_ERR_OR_NULL(sf->file)) {
		dma_fence_put(sf->fence);
		kfree(sf);
		return NULL;
	}
	return sf;
}

struct dma_fence *sync_file_get_fence(int fd)
{
	struct file *file = fget(fd);
	struct dma_fence *fence = NULL;
	if (!file)
		return NULL;
	if (file->f_op == &sync_file_operations) {
		struct sync_file *sf = file->private_data;
		fence = dma_fence_get(sf->fence);
	}
	fput(file);
	return fence;
}

void sync_file_put(struct sync_file *sf)
{
	if (sf) fput(sf->file);
}

void sync_file_destroy(struct sync_file *sf)
{
	sync_file_put(sf);
}
