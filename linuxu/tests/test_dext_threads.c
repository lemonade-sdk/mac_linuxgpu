/* Mock DriverKit dispatch/TLS with native host threads.
 * Build: clang -std=gnu11 -DLINUXU_TEST_DEXT_THREADS=1
 *   linuxu/tests/test_dext_threads.c linuxu/src/shims/dext_threads.c
 *   -lpthread -o /tmp/test_dext_threads && /tmp/test_dext_threads
 * To exercise the DriverKit synchronization adapters too, compile
 * dext_threads.c and dext_sync.c separately with
 *   -include linuxu/tests/dext_sync_rename.h
 * and their respective LINUXU_TEST_DEXT_THREADS/SYNC definitions, then link
 * test_dext_threads.c with those objects and test_dext_sync.c compiled with
 *   -Dmain=unused_sync_fixture_main
 * for the IOLock/IOSleep mock backend.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

extern int dext_test_pthread_create(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *);
extern int dext_test_pthread_join(pthread_t, void **);
extern pthread_t dext_test_pthread_self(void);
extern int dext_test_pthread_equal(pthread_t, pthread_t);
extern int dext_thread_identity_swap(uint64_t, uint64_t *);
extern void dext_thread_task_run(void *);
extern void dext_thread_task_fail(void *);
extern int linuxu_test_iolock_fail_after;

static _Thread_local void *identity;
static int fail_identity;
int IOThreadLocalStorageKeyCreate(uint64_t *key) { *key = 1; return 0; }
int IOThreadLocalStorageSet(uint64_t key, const void *value) {
    assert(key == 1);
    if (fail_identity && value) return EAGAIN;
    identity = (void *)value;
    return 0;
}
void *IOThreadLocalStorageGet(uint64_t key) {
    assert(key == 1);
    return identity;
}

struct scheduled_task { void *task; uint64_t identity; };
struct join_task { uint64_t identity; int result; };
static int join_before_schedule_returns;
static void *join_scheduled(void *opaque) {
    struct join_task *join = opaque;
    join->result = dext_test_pthread_join((pthread_t)(uintptr_t)join->identity,
                                          NULL);
    return NULL;
}
static void *run_scheduled(void *opaque) {
    struct scheduled_task *scheduled = opaque;
    uint64_t previous = 0, ignored = 0;
    if (dext_thread_identity_swap(scheduled->identity, &previous) == 0) {
        dext_thread_task_run(scheduled->task);
        (void)dext_thread_identity_swap(previous, &ignored);
    } else {
        dext_thread_task_fail(scheduled->task);
    }
    free(scheduled);
    return NULL;
}
int dext_thread_schedule(void *task, uint64_t thread_identity) {
    pthread_t native;
    struct scheduled_task *scheduled = malloc(sizeof(*scheduled));
    if (!scheduled) return ENOMEM;
    scheduled->task = task;
    scheduled->identity = thread_identity;
    if (pthread_create(&native, NULL, run_scheduled, scheduled)) {
        free(scheduled);
        return EAGAIN;
    }
    if (join_before_schedule_returns) {
        pthread_t joiner;
        struct join_task join = { .identity = thread_identity, .result = -1 };
        assert(pthread_create(&joiner, NULL, join_scheduled, &join) == 0);
        assert(pthread_join(joiner, NULL) == 0);
        assert(pthread_join(native, NULL) == 0);
        assert(join.result == 0);
        return 0;
    }
    pthread_detach(native);
    return 0;
}

static void *worker(void *argument) {
    pthread_t *observed = argument;
    *observed = dext_test_pthread_self();
    assert(dext_test_pthread_equal(*observed, dext_test_pthread_self()));
    return argument;
}

int main(void) {
    pthread_t handle = 0, worker_identity = 0;
    pthread_attr_t unsupported_attr;
    void *result = NULL;
    /* The completion lock succeeds, but the registry's IOLock allocation
     * fails. Its allocation-free mutex fallback must still protect slots. */
    linuxu_test_iolock_fail_after = 2;
    assert(dext_test_pthread_create(&handle, NULL, worker,
                                    &worker_identity) == 0);
    assert(dext_test_pthread_join(handle, NULL) == 0);
    assert(worker_identity == handle);
    handle = 0;
    linuxu_test_iolock_fail_after = 0;
    pthread_t parent = dext_test_pthread_self();
    assert(parent != 0);
    assert(dext_test_pthread_self() == parent);
    assert(dext_test_pthread_equal(parent, dext_test_pthread_self()));
    assert(dext_test_pthread_create(&handle, &unsupported_attr,
                                    worker, &worker_identity) == ENOTSUP);
    assert(dext_test_pthread_create(&handle, NULL, worker,
                                    &worker_identity) == 0);
    assert(dext_test_pthread_equal(handle, handle));
    assert(!dext_test_pthread_equal(handle, parent));
    assert(!dext_test_pthread_equal(parent, handle));
    assert(dext_test_pthread_join(handle, &result) == 0);
    assert(result == &worker_identity);
    assert(worker_identity == handle);
    assert(dext_test_pthread_self() == parent);
    assert(dext_test_pthread_join(handle, NULL) == ESRCH);

    pthread_t second = 0;
    assert(dext_test_pthread_create(&second, NULL, worker,
                                    &worker_identity) == 0);
    assert(second != handle); /* generation rejects reused slot handles */
    assert(dext_test_pthread_join(second, NULL) == 0);

    join_before_schedule_returns = 1;
    assert(dext_test_pthread_create(&second, NULL, worker,
                                    &worker_identity) == 0);
    join_before_schedule_returns = 0;
    assert(worker_identity == second);
    assert(dext_test_pthread_join(second, NULL) == ESRCH);

    pthread_t slots[128] = {0};
    pthread_t observed[128] = {0};
    for (unsigned int i = 0; i < 128; i++)
        assert(dext_test_pthread_create(&slots[i], NULL, worker,
                                        &observed[i]) == 0);
    assert(dext_test_pthread_create(&second, NULL, worker,
                                    &worker_identity) == EAGAIN);
    assert(dext_test_pthread_equal(slots[0], slots[0]));
    assert(!dext_test_pthread_equal(slots[0], slots[127]));
    for (unsigned int i = 0; i < 128; i++) {
        assert(dext_test_pthread_join(slots[i], NULL) == 0);
        assert(observed[i] == slots[i]);
    }
    fail_identity = 1;
    handle = 0;
    assert(dext_test_pthread_create(&handle, NULL, worker, &worker_identity) == EAGAIN);
    assert(!handle);
    fail_identity = 0;
    assert(dext_test_pthread_create(&handle, NULL, worker, &worker_identity) == 0);
    assert(dext_test_pthread_join(handle, NULL) == 0);
    return 0;
}
