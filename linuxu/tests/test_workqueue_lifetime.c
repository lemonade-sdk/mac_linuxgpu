/* Callback-owned work may disappear before the callback returns. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic int fail_cond_init, fail_join, fail_create;
static int work_test_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*entry)(void *), void *data)
{
    return fail_create ? EAGAIN : pthread_create(thread, attr, entry, data);
}
static int work_test_cond_init(pthread_cond_t *cv, const pthread_condattr_t *attr)
{
    return fail_cond_init ? ENOMEM : pthread_cond_init(cv, attr);
}
static int work_test_join(pthread_t thread, void **value)
{
    if (fail_join) { fail_join = 0; return EAGAIN; }
    return pthread_join(thread, value);
}
#define pthread_cond_init work_test_cond_init
#define pthread_join work_test_join
#define pthread_create work_test_create
#include "../src/work.c"
#undef pthread_cond_init
#undef pthread_join
#undef pthread_create

/* work.c reports through printk; capture it instead of linking klog. */
static atomic_int printk_calls;
static char printk_last[512];
int printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(printk_last, sizeof(printk_last), fmt, args);
    va_end(args);
    fputs(printk_last, stderr);
    return atomic_fetch_add_explicit(&printk_calls, 1, memory_order_seq_cst) + 1;
}

static atomic_int started, permitted, returned, flushed;
static struct workqueue_struct *test_queue;
static void wait_value(atomic_int *value, int expected)
{
    for (int i = 0; i < 10000; ++i) {
        if (atomic_load(value) == expected) return;
        usleep(100);
    }
    assert(!"worker synchronization timed out");
}
static void self_free(struct work_struct *work)
{
    atomic_store(&started, 1);
    wait_value(&permitted, 1);
    free(work);
    atomic_store(&returned, 1);
}
static void *flush_one(void *work)
{
    flush_work(work);
    assert(atomic_load(&returned));
    atomic_store(&flushed, 1);
    return NULL;
}
static void *cancel_one(void *work)
{
    assert(!cancel_work_sync(work)); /* Running alone is not pending. */
    assert(atomic_load(&returned));
    return NULL;
}
static void *flush_delayed_one(void *work)
{
    flush_delayed_work(work);
    assert(atomic_load(&returned));
    return NULL;
}
static void await_barrier(struct work_struct *work, bool cancel)
{
    for (int i = 0; i < 10000; ++i) {
        pthread_mutex_lock(&work_lock);
        struct work_execution *run = running_work(work);
        bool waiting = cancel ? (run && run->cancelling) : barriers != NULL;
        pthread_mutex_unlock(&work_lock);
        if (waiting) return;
        usleep(100);
    }
    assert(!"barrier was not registered");
}
static void test_free_during_barrier(bool cancel)
{
    pthread_t waiter;
    struct work_struct *work = malloc(sizeof(*work));
    assert(work);
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    atomic_store(&returned, 0); atomic_store(&flushed, 0);
    INIT_WORK(work, self_free);
    assert(queue_work(test_queue, work));
    wait_value(&started, 1);
    assert(pthread_create(&waiter, NULL, cancel ? cancel_one : flush_one, work) == 0);
    await_barrier(work, cancel);
    atomic_store(&permitted, 1);
    assert(pthread_join(waiter, NULL) == 0);
    flush_workqueue(test_queue);
}
static void test_delayed_self_free(void)
{
    pthread_t waiter;
    struct delayed_work *work = malloc(sizeof(*work));
    assert(work);
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    atomic_store(&returned, 0);
    INIT_DELAYED_WORK(work, self_free);
    assert(queue_delayed_work(test_queue, work, HZ * 60));
    assert(pthread_create(&waiter, NULL, flush_delayed_one, work) == 0);
    wait_value(&started, 1);
    await_barrier(&work->work, false);
    atomic_store(&permitted, 1);
    assert(pthread_join(waiter, NULL) == 0);
    flush_workqueue(test_queue);
}
static int requeue_turn;
static void requeue(struct work_struct *work)
{
    int turn = ++requeue_turn;
    if (turn < 3) assert(queue_work(test_queue, work));
    atomic_store(&started, turn);
    wait_value(&permitted, turn);
    atomic_store(&returned, 1);
}
static void test_flush_visible_generations(void)
{
    struct work_struct work;
    pthread_t waiter;
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    atomic_store(&returned, 0); atomic_store(&flushed, 0);
    INIT_WORK(&work, requeue);
    assert(queue_work(test_queue, &work));
    wait_value(&started, 1);
    /* First callback publishes its next execution before waiting. */
    pthread_mutex_lock(&work_lock);
    assert(state(&work) & WORK_PENDING);
    pthread_mutex_unlock(&work_lock);
    assert(pthread_create(&waiter, NULL, flush_one, &work) == 0);
    await_barrier(&work, false);
    atomic_store(&permitted, 1);
    wait_value(&started, 2);
    assert(!atomic_load(&flushed));
    atomic_store(&permitted, 2);
    wait_value(&started, 3);
    wait_value(&flushed, 1); /* Third execution was queued after flush began. */
    atomic_store(&permitted, 3);
    assert(pthread_join(waiter, NULL) == 0);
    flush_workqueue(test_queue);
}
static int stop_turn;
static void self_free_on_stop(struct work_struct *work)
{
    if (++stop_turn == 1) {
        atomic_store(&started, 1);
        wait_value(&permitted, 1);
        /* Linux lets a queue's own items chain work while it is destroyed. */
        assert(queue_work(test_queue, work));
        return;
    }
    free(work);
    atomic_store(&returned, 1);
}
static void *destroy_queue(void *queue)
{
    destroy_workqueue(queue);
    return NULL;
}
static void test_destroy_drains_callback(void)
{
    struct work_struct *work = malloc(sizeof(*work));
    struct work_struct outsider;
    pthread_t destroyer;
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    atomic_store(&returned, 0);
    stop_turn = 0;
    INIT_WORK(work, self_free_on_stop);
    INIT_WORK(&outsider, self_free_on_stop);
    assert(queue_work(test_queue, work));
    wait_value(&started, 1);
    assert(pthread_create(&destroyer, NULL, destroy_queue, test_queue) == 0);
    for (;;) {
        pthread_mutex_lock(&work_lock);
        bool stopping = test_queue->stop;
        pthread_mutex_unlock(&work_lock);
        if (stopping) break;
        usleep(100);
    }
    /* Submissions from outside the queue are refused once destroy begins. */
    int warnings = atomic_load(&printk_calls);
    assert(!queue_work(test_queue, &outsider));
    assert(atomic_load(&printk_calls) > warnings);
    atomic_store(&permitted, 1);
    assert(pthread_join(destroyer, NULL) == 0);
    assert(atomic_load(&returned)); /* The chained run finished first. */
}
static void no_work(struct work_struct *work) { (void)work; }
static atomic_int cancelled_calls;
static void counted_work(struct work_struct *work)
{
    (void)work;
    atomic_fetch_add_explicit(&cancelled_calls, 1, memory_order_seq_cst);
}
static void block_queue(struct work_struct *work)
{
    (void)work;
    atomic_store(&started, 1);
    wait_value(&permitted, 1);
}
static void test_async_cancel(void)
{
    struct work_struct blocker, pending;
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    INIT_WORK(&blocker, block_queue);
    INIT_WORK(&pending, counted_work);
    assert(queue_work(test_queue, &blocker));
    wait_value(&started, 1);
    assert(work_busy(&blocker) == WORK_BUSY_RUNNING);
    assert(!cancel_work(&blocker));
    assert(queue_work(test_queue, &pending));
    assert(work_busy(&pending) == WORK_BUSY_PENDING);
    assert(cancel_work(&pending));
    assert(!work_busy(&pending));
    assert(!cancel_work(&pending));
    assert(!flush_work(&pending));
    assert(queue_work(test_queue, &pending));
    assert(cancel_work_sync(&pending));
    atomic_store(&permitted, 1);
    flush_workqueue(test_queue);
    assert(atomic_load(&cancelled_calls) == 0);
    assert(queue_work(test_queue, &pending));
    flush_workqueue(test_queue);
    assert(atomic_load(&cancelled_calls) == 1);
}
static atomic_int later_started, later_permitted;
static void later_work(struct work_struct *work)
{
    (void)work;
    atomic_store(&later_started, 1);
    wait_value(&later_permitted, 1);
}
static void *flush_queue(void *queue)
{
    flush_workqueue(queue);
    atomic_store(&flushed, 1);
    return NULL;
}
static void test_queue_flush_snapshot(void)
{
    struct work_struct earlier, later;
    pthread_t waiter;
    atomic_store(&started, 0); atomic_store(&permitted, 0);
    atomic_store(&flushed, 0);
    atomic_store(&later_started, 0); atomic_store(&later_permitted, 0);
    INIT_WORK(&earlier, block_queue);
    INIT_WORK(&later, later_work);
    assert(queue_work(test_queue, &earlier));
    wait_value(&started, 1);
    assert(!pthread_create(&waiter, NULL, flush_queue, test_queue));
    for (int i = 0;; ++i) { /* The flusher has taken its snapshot. */
        pthread_mutex_lock(&work_lock);
        bool waiting = queue_flushers != 0;
        pthread_mutex_unlock(&work_lock);
        if (waiting) break;
        assert(i < 10000);
        usleep(100);
    }
    assert(queue_work(test_queue, &later));
    atomic_store(&permitted, 1);
    wait_value(&later_started, 1);
    wait_value(&flushed, 1); /* Later work is still blocked. */
    assert(!pthread_join(waiter, NULL));
    atomic_store(&later_permitted, 1);
    drain_workqueue(test_queue);
}
static atomic_int delayed_ran;
static void delayed_marker(struct work_struct *work)
{
    (void)work;
    atomic_fetch_add_explicit(&delayed_ran, 1, memory_order_seq_cst);
}
/* Linux never runs timer-pending work early at destroy; the caller should
 * have cancelled it.  It is dropped with a warning and never touched again. */
static void test_destroy_pending_delayed(void)
{
    struct workqueue_struct *queue = alloc_workqueue("pending-delayed", 0, 0);
    struct delayed_work *dw = malloc(sizeof(*dw));
    assert(queue && dw);
    atomic_store(&delayed_ran, 0);
    INIT_DELAYED_WORK(dw, delayed_marker);
    assert(queue_delayed_work(queue, dw, HZ / 5));
    int warnings = atomic_load(&printk_calls);
    destroy_workqueue(queue);
    assert(atomic_load(&printk_calls) > warnings);
    assert(!work_pending(&dw->work));
    assert(!flush_delayed_work(dw));
    usleep(400 * 1000);
    assert(atomic_load(&delayed_ran) == 0);
    free(dw);
}
/* A worker that cannot be started is retried; the work is never dropped. */
static void test_spawn_failure_retries(void)
{
    struct workqueue_struct *queue = alloc_workqueue("spawn-retry", 0, 0);
    struct work_struct work;
    assert(queue);
    atomic_store(&cancelled_calls, 0);
    INIT_WORK(&work, counted_work);
    int warnings = atomic_load(&printk_calls);
    fail_create = 1;
    assert(queue_work(queue, &work));
    usleep(200 * 1000);
    assert(atomic_load(&cancelled_calls) == 0);
    assert(work_pending(&work));
    assert(atomic_load(&printk_calls) > warnings);
    fail_create = 0;
    flush_work(&work);
    assert(atomic_load(&cancelled_calls) == 1);
    destroy_workqueue(queue);
}
static void test_null_queue_is_reported(void)
{
    struct work_struct work;
    struct delayed_work dw;
    atomic_store(&cancelled_calls, 0);
    INIT_WORK(&work, counted_work);
    INIT_DELAYED_WORK(&dw, counted_work);
    int warnings = atomic_load(&printk_calls);
    assert(queue_work(NULL, &work));
    assert(atomic_load(&printk_calls) == warnings + 1);
    assert(strstr(printk_last, "queue_work") && strstr(printk_last, "NULL"));
    flush_work(&work);
    assert(queue_delayed_work(NULL, &dw, 1));
    assert(strstr(printk_last, "queue_delayed_work"));
    flush_delayed_work(&dw);
    assert(!mod_delayed_work(NULL, &dw, 0));
    assert(strstr(printk_last, "mod_delayed_work"));
    flush_delayed_work(&dw);
    assert(atomic_load(&cancelled_calls) == 3);
}
/* Idle workers give their thread back and are joined; work queued later
 * gets a fresh worker. */
static void test_idle_workers_retire(void)
{
    struct workqueue_struct *queue = alloc_workqueue("retire", 0, 4);
    struct work_struct works[4];
    bool retired = false;
    assert(queue);
    pthread_mutex_lock(&work_lock);
    worker_idle_ms = 50;
    pthread_mutex_unlock(&work_lock);
    atomic_store(&cancelled_calls, 0);
    for (int i = 0; i < 4; ++i) {
        INIT_WORK(&works[i], counted_work);
        assert(queue_work(queue, &works[i]));
    }
    flush_workqueue(queue);
    assert(atomic_load(&cancelled_calls) == 4);
    for (int i = 0; i < 20000 && !retired; ++i) {
        pthread_mutex_lock(&work_lock);
        retired = !queue->nr_workers && !queue->zombies && !queue->nr_reaping;
        pthread_mutex_unlock(&work_lock);
        if (!retired) usleep(100);
    }
    assert(retired);
    assert(queue_work(queue, &works[0]));
    flush_work(&works[0]);
    assert(atomic_load(&cancelled_calls) == 5);
    pthread_mutex_lock(&work_lock);
    worker_idle_ms = LINUXU_WQ_IDLE_MS;
    pthread_mutex_unlock(&work_lock);
    destroy_workqueue(queue);
}
int main(void)
{
    fail_create = 1;
    assert(linuxu_workqueue_init() == -EAGAIN); /* manager cannot start */
    assert(!system_wq && !alloc_workqueue("no-manager", 0, 0));
    fail_create = 0;
    assert(linuxu_workqueue_init() == 0);
    assert(linuxu_workqueue_init() == 0); /* idempotent */
    assert(system_wq && system_dfl_wq && system_unbound_wq &&
           system_freezable_wq && system_highpri_wq && system_long_wq);
    assert(system_wq != system_dfl_wq); /* independent pools */
    fail_cond_init = 1;
    assert(!alloc_workqueue("failed-condition", 0, 1));
    fail_cond_init = 0;
    test_queue = alloc_workqueue("callback-lifetime", 0, 1); assert(test_queue);
    assert(workqueue_is_single_threaded(test_queue));
    test_async_cancel();
    test_queue_flush_snapshot();
    test_free_during_barrier(false);
    test_free_during_barrier(true);
    test_delayed_self_free();
    test_flush_visible_generations();
    test_destroy_drains_callback();
    test_queue = alloc_workqueue("join-failure", 0, 1); assert(test_queue);
    struct work_struct work;
    INIT_WORK(&work, no_work); assert(queue_work(test_queue, &work));
    flush_work(&work);
    fail_join = 1;
    destroy_workqueue(test_queue);
    assert(test_queue->stop); /* Failed join must not free worker storage. */
    destroy_workqueue(test_queue);
    test_destroy_pending_delayed();
    test_spawn_failure_retries();
    test_null_queue_is_reported();
    test_idle_workers_retire();
    puts("workqueue self-free, flush, cancel, shutdown, delayed destroy, "
         "spawn retry, NULL queue, idle retirement and allocation failures passed");
}
