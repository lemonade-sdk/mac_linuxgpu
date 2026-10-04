/* Cross-thread RCU grace periods for the one-device DriverKit runtime. */
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <linux/rcupdate.h>
#include <rt/fatal.h>

#ifdef LINUXU_DEXT_DK
extern int IOThreadLocalStorageKeyCreate(uint64_t *key);
extern int IOThreadLocalStorageSet(uint64_t key, const void *value);
extern void *IOThreadLocalStorageGet(uint64_t key);
static pthread_once_t rcu_tls_once = PTHREAD_ONCE_INIT;
static uint64_t rcu_tls_key;
static int rcu_tls_ready;
static void rcu_tls_init(void)
{
	rcu_tls_ready = IOThreadLocalStorageKeyCreate(&rcu_tls_key) == 0;
}
static uintptr_t rcu_tls_get(void)
{
	if (pthread_once(&rcu_tls_once, rcu_tls_init) || !rcu_tls_ready)
		/* unable to track a reader: never reclaim */
		LINUXU_FATAL("rcu: reader TLS key unavailable");
	return (uintptr_t)IOThreadLocalStorageGet(rcu_tls_key);
}
static void rcu_tls_set(uintptr_t value)
{
	if (IOThreadLocalStorageSet(rcu_tls_key, (const void *)value))
		LINUXU_FATAL("rcu: reader TLS store failed");
}
#else
static __thread uintptr_t rcu_tls_state;
static uintptr_t rcu_tls_get(void) { return rcu_tls_state; }
static void rcu_tls_set(uintptr_t value) { rcu_tls_state = value; }
#endif

/* TLS state packs nesting in bits 1.. and the reader epoch in bit 0.
 *
 * Readers take no lock: an outermost rcu_read_lock counts itself in its
 * epoch's counter and checks the epoch did not flip meanwhile (else it
 * moves to the new one), and rcu_read_unlock uncounts itself. A grace
 * period flips the epoch and waits for the old epoch's counter to drain;
 * a reader that empties a counter wakes it only when one is waiting.
 * With sequentially consistent atomics on both sides, a reader the grace
 * period's check missed sees the flip in its own recheck. Readers are on
 * every fence, syncobj and scheduler path, so they must not serialize on a
 * mutex or signal a condition per call. */
static pthread_mutex_t rcu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rcu_changed = PTHREAD_COND_INITIALIZER;
static pthread_cond_t rcu_pending = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t rcu_gp_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int rcu_epoch;
static uint64_t rcu_readers[2];
static unsigned int rcu_gp_waiting;
static struct rcu_head *rcu_first;
static struct rcu_head *rcu_last;
static uint64_t rcu_enqueued;
static uint64_t rcu_completed;
static int rcu_worker_started;
static int rcu_worker_idle;
static pthread_t rcu_worker;

/* Callbacks wait this long before their grace period, so the ones queued
 * meanwhile share it (and the worker's wakeup). Linux batches the same. */
#define RCU_BATCH_NS 1000000ull

void rcu_read_lock(void)
{
	uintptr_t state = rcu_tls_get();
	uintptr_t nesting = state >> 1;
	unsigned int epoch;

	if (nesting) {
		if (nesting == (UINTPTR_MAX >> 1))
			LINUXU_FATAL("rcu_read_lock nesting overflow");
		rcu_tls_set(((nesting + 1) << 1) | (state & 1));
		return;
	}
	for (;;) {
		epoch = __atomic_load_n(&rcu_epoch, __ATOMIC_SEQ_CST);
		__atomic_add_fetch(&rcu_readers[epoch], 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&rcu_epoch, __ATOMIC_SEQ_CST) == epoch)
			break;
		/* Flipped meanwhile: count in the new epoch instead. */
		if (!__atomic_sub_fetch(&rcu_readers[epoch], 1, __ATOMIC_SEQ_CST) &&
		    __atomic_load_n(&rcu_gp_waiting, __ATOMIC_SEQ_CST)) {
			pthread_mutex_lock(&rcu_lock);
			pthread_cond_broadcast(&rcu_changed);
			pthread_mutex_unlock(&rcu_lock);
		}
	}
	rcu_tls_set(((uintptr_t)1 << 1) | epoch);
}

void rcu_read_unlock(void)
{
	uintptr_t state = rcu_tls_get();
	uintptr_t nesting = state >> 1;
	unsigned int epoch = (unsigned int)(state & 1);
	uint64_t left;

	if (!nesting)
		LINUXU_FATAL("rcu_read_unlock without rcu_read_lock");
	if (nesting > 1) {
		rcu_tls_set(((nesting - 1) << 1) | epoch);
		return;
	}
	rcu_tls_set(0);
	left = __atomic_sub_fetch(&rcu_readers[epoch], 1, __ATOMIC_SEQ_CST);
	if (left == UINT64_MAX)
		LINUXU_FATAL("rcu reader count underflow");
	if (!left && __atomic_load_n(&rcu_gp_waiting, __ATOMIC_SEQ_CST)) {
		pthread_mutex_lock(&rcu_lock);
		pthread_cond_broadcast(&rcu_changed);
		pthread_mutex_unlock(&rcu_lock);
	}
}

int rcu_read_lock_count(void) { return (int)(rcu_tls_get() >> 1); }
void rcu_defer_init_thread(void) { (void)rcu_tls_get(); }

/* Take a FIFO batch, flip the entry epoch, and wait only for earlier readers.
 * Callbacks run in order; next is saved before a callback can free its head. */
static void rcu_grace_period(void)
{
	struct rcu_head *head, *next;
	unsigned int old;
	pthread_mutex_lock(&rcu_gp_lock);
	pthread_mutex_lock(&rcu_lock);
	head = rcu_first;
	rcu_first = rcu_last = NULL;
	old = __atomic_load_n(&rcu_epoch, __ATOMIC_SEQ_CST);
	__atomic_store_n(&rcu_epoch, old ^ 1, __ATOMIC_SEQ_CST);
	/* Before the check below: a reader that drains the counter after it
	 * sees this and wakes the wait (it takes rcu_lock, held until the
	 * wait releases it). */
	__atomic_store_n(&rcu_gp_waiting, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&rcu_readers[old], __ATOMIC_SEQ_CST))
		pthread_cond_wait(&rcu_changed, &rcu_lock);
	__atomic_store_n(&rcu_gp_waiting, 0, __ATOMIC_SEQ_CST);
	pthread_mutex_unlock(&rcu_lock);
	while (head) {
		next = head->next;
		head->func(head);
		pthread_mutex_lock(&rcu_lock);
		rcu_completed++;
		pthread_cond_broadcast(&rcu_changed);
		pthread_mutex_unlock(&rcu_lock);
		head = next;
	}
	pthread_mutex_unlock(&rcu_gp_lock);
}

static void *rcu_worker_main(void *unused)
{
	(void)unused;
	for (;;) {
		struct timespec batch = { 0, (long)RCU_BATCH_NS };

		pthread_mutex_lock(&rcu_lock);
		while (!rcu_first) {
			rcu_worker_idle = 1;
			pthread_cond_wait(&rcu_pending, &rcu_lock);
			rcu_worker_idle = 0;
		}
		/* Let the callbacks queued in the next moment share this grace
		 * period. A barrier or synchronize_rcu runs its own. */
		(void)pthread_cond_timedwait_relative_np(&rcu_pending, &rcu_lock, &batch);
		pthread_mutex_unlock(&rcu_lock);
		rcu_grace_period();
	}
}

static int rcu_start_worker_locked(void)
{
	int ret;
	if (rcu_worker_started)
		return 0;
	ret = pthread_create(&rcu_worker, NULL, rcu_worker_main, NULL);
	if (!ret)
		rcu_worker_started = 1;
	return ret;
}

int rcu_spawn_gp_kthread(void)
{
	int ret;
	pthread_mutex_lock(&rcu_lock);
	ret = rcu_start_worker_locked();
	pthread_mutex_unlock(&rcu_lock);
	return ret ? -ret : 0;
}

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
	if (!head || !func)
		LINUXU_FATAL("call_rcu with NULL head or callback");
	head->func = func;
	head->next = NULL;
	pthread_mutex_lock(&rcu_lock);
	if (rcu_last)
		rcu_last->next = head;
	else
		rcu_first = head;
	rcu_last = head;
	rcu_enqueued++;
	/* Startup failure leaves callbacks queued for a later retry or barrier. */
	(void)rcu_start_worker_locked();
	/* Only an idle worker needs the wake; a busy one takes this batch next. */
	if (rcu_worker_idle)
		pthread_cond_signal(&rcu_pending);
	pthread_mutex_unlock(&rcu_lock);
}

static void rcu_wait_callbacks(uint64_t target)
{
	for (;;) {
		int pending;
		pthread_mutex_lock(&rcu_lock);
		if (rcu_completed >= target) {
			pthread_mutex_unlock(&rcu_lock);
			return;
		}
		pending = rcu_first != NULL;
		if (!pending)
			pthread_cond_wait(&rcu_changed, &rcu_lock);
		pthread_mutex_unlock(&rcu_lock);
		if (pending)
			rcu_grace_period();
	}
}

void synchronize_rcu(void)
{
	uint64_t target;
	pthread_mutex_lock(&rcu_lock);
	target = rcu_enqueued;
	pthread_mutex_unlock(&rcu_lock);
	rcu_grace_period();
	rcu_wait_callbacks(target);
}
void synchronize_rcu_expedited(void) { synchronize_rcu(); }
void synchronize_sched(void) { synchronize_rcu(); }
void synchronize_rcu_bh(void) { synchronize_rcu(); }

void rcu_barrier(void)
{
	uint64_t target;
	pthread_mutex_lock(&rcu_lock);
	target = rcu_enqueued;
	pthread_mutex_unlock(&rcu_lock);
	rcu_wait_callbacks(target);
}
void rcu_barrier_bh(void) { rcu_barrier(); }
void rcu_barrier_sched(void) { rcu_barrier(); }

/* SRCU sections can sleep and can wait for a different SRCU domain. Keep
 * grace-period serialization in each domain; holding a global GP mutex
 * across a reader wait would deadlock such cross-domain dependencies. */
static pthread_mutex_t srcu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t srcu_changed = PTHREAD_COND_INITIALIZER;

int init_srcu_struct(struct srcu_struct *ssp)
{
	ssp->srcu_ctrp = NULL;
	ssp->srcu_reader_flavor = 0;
	ssp->readers[0] = ssp->readers[1] = 0;
	ssp->epoch = 0;
	ssp->grace_period_active = false;
	return 0;
}

void srcu_init_struct(struct srcu_struct *ssp)
{
	(void)init_srcu_struct(ssp);
}

int srcu_read_lock(struct srcu_struct *ssp)
{
	pthread_mutex_lock(&srcu_lock);
	unsigned int epoch = ssp->epoch;
	ssp->readers[epoch]++;
	pthread_mutex_unlock(&srcu_lock);
	return (int)epoch;
}

void srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	pthread_mutex_lock(&srcu_lock);
	if ((unsigned int)idx > 1 || !ssp->readers[idx]) {
		pthread_mutex_unlock(&srcu_lock);
		LINUXU_FATAL("srcu_read_unlock with invalid index");
	}
	if (!--ssp->readers[idx])
		pthread_cond_broadcast(&srcu_changed);
	pthread_mutex_unlock(&srcu_lock);
}

void synchronize_srcu(struct srcu_struct *ssp)
{
	pthread_mutex_lock(&srcu_lock);
	while (ssp->grace_period_active)
		pthread_cond_wait(&srcu_changed, &srcu_lock);
	ssp->grace_period_active = true;
	unsigned int old = ssp->epoch;
	ssp->epoch ^= 1;
	while (ssp->readers[old])
		pthread_cond_wait(&srcu_changed, &srcu_lock);
	ssp->grace_period_active = false;
	pthread_cond_broadcast(&srcu_changed);
	pthread_mutex_unlock(&srcu_lock);
}

void synchronize_srcu_expedited(struct srcu_struct *ssp)
{
	synchronize_srcu(ssp);
}

void cleanup_srcu_struct(struct srcu_struct *ssp)
{
	/* The caller has already stopped new readers before destroying a domain. */
	synchronize_srcu(ssp);
}

/* call_srcu: one FIFO and one worker for all SRCU domains. A batch waits
 * for a grace period of each domain it contains before running callbacks,
 * in queue order. The worker never runs inside an SRCU reader, so a
 * callback queued from a reader (mmu_notifier_put from ->release) cannot
 * deadlock its own grace period. */
static pthread_mutex_t srcu_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t srcu_cb_pending = PTHREAD_COND_INITIALIZER;
static pthread_cond_t srcu_cb_done = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t srcu_cb_run_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rcu_head *srcu_cb_first, *srcu_cb_last;
static uint64_t srcu_cb_enqueued, srcu_cb_completed;
static int srcu_cb_worker_started;
static pthread_t srcu_cb_worker;

/* Take and run one batch; returns false when nothing was queued. */
static bool srcu_cb_run_batch(void)
{
	struct rcu_head *head, *next;

	pthread_mutex_lock(&srcu_cb_run_lock);
	pthread_mutex_lock(&srcu_cb_lock);
	head = srcu_cb_first;
	srcu_cb_first = srcu_cb_last = NULL;
	pthread_mutex_unlock(&srcu_cb_lock);
	if (!head) {
		pthread_mutex_unlock(&srcu_cb_run_lock);
		return false;
	}
	for (struct rcu_head *h = head; h; h = h->next) {
		bool seen = false;

		for (struct rcu_head *p = head; p != h; p = p->next)
			if (p->linuxu_srcu == h->linuxu_srcu) { seen = true; break; }
		if (!seen)
			synchronize_srcu(h->linuxu_srcu);
	}
	while (head) {
		next = head->next;
		head->func(head);
		pthread_mutex_lock(&srcu_cb_lock);
		srcu_cb_completed++;
		pthread_cond_broadcast(&srcu_cb_done);
		pthread_mutex_unlock(&srcu_cb_lock);
		head = next;
	}
	pthread_mutex_unlock(&srcu_cb_run_lock);
	return true;
}

static void *srcu_cb_worker_main(void *unused)
{
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&srcu_cb_lock);
		while (!srcu_cb_first)
			pthread_cond_wait(&srcu_cb_pending, &srcu_cb_lock);
		pthread_mutex_unlock(&srcu_cb_lock);
		srcu_cb_run_batch();
	}
	return NULL;
}

void call_srcu(struct srcu_struct *ssp, struct rcu_head *head,
	       void (*func)(struct rcu_head *head))
{
	if (!ssp || !head || !func)
		LINUXU_FATAL("call_srcu with NULL domain, head or callback");
	head->func = func;
	head->next = NULL;
	head->linuxu_srcu = ssp;
	pthread_mutex_lock(&srcu_cb_lock);
	if (srcu_cb_last)
		srcu_cb_last->next = head;
	else
		srcu_cb_first = head;
	srcu_cb_last = head;
	srcu_cb_enqueued++;
	/* Startup failure leaves callbacks for srcu_barrier to run. */
	if (!srcu_cb_worker_started &&
	    !pthread_create(&srcu_cb_worker, NULL, srcu_cb_worker_main, NULL))
		srcu_cb_worker_started = 1;
	pthread_cond_signal(&srcu_cb_pending);
	pthread_mutex_unlock(&srcu_cb_lock);
}

void srcu_barrier(struct srcu_struct *ssp)
{
	uint64_t target;

	(void)ssp;
	pthread_mutex_lock(&srcu_cb_lock);
	target = srcu_cb_enqueued;
	while (srcu_cb_completed < target) {
		if (!srcu_cb_worker_started && srcu_cb_first) {
			pthread_mutex_unlock(&srcu_cb_lock);
			srcu_cb_run_batch();
			pthread_mutex_lock(&srcu_cb_lock);
			continue;
		}
		pthread_cond_wait(&srcu_cb_done, &srcu_cb_lock);
	}
	pthread_mutex_unlock(&srcu_cb_lock);
}
