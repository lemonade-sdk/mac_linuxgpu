/* DriverKit task identity failures must neither enter a worker without its
 * stop identity nor free an identity still published on a reused thread. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/kthread.h>

static _Thread_local void *task_slot;
static int fail_create, fail_set_call;
static atomic_int set_calls, entry_calls;
static struct task_struct *failed_restore_task;
int IOThreadLocalStorageKeyCreate(uint64_t *key)
{
    if (fail_create) return EAGAIN;
    *key = 1;
    return 0;
}
void *IOThreadLocalStorageGet(uint64_t key)
{ assert(key == 1); return task_slot; }
int IOThreadLocalStorageSet(uint64_t key, const void *value)
{
    assert(key == 1);
    int call = atomic_fetch_add_explicit(&set_calls, 1, memory_order_seq_cst) + 1;
    if (call == fail_set_call) {
        if (!value) failed_restore_task = task_slot;
        return EAGAIN;
    }
    task_slot = (void *)value;
    return 0;
}
static int entry(void *unused)
{
    (void)unused;
    assert(current->flags & PF_KTHREAD);
    atomic_fetch_add_explicit(&entry_calls, 1, memory_order_seq_cst);
    return 37;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    alarm(10);
    fail_create = !strcmp(argv[1], "create");
    fail_set_call = !strcmp(argv[1], "publish") ? 1 :
                    !strcmp(argv[1], "restore") ? 2 : 0;
    struct task_struct *task = kthread_create(entry, NULL, "tls-failure");
    if (fail_create || fail_set_call == 1) {
        assert(IS_ERR(task) && PTR_ERR(task) == -EAGAIN);
        assert(atomic_load(&entry_calls) == 0);
        puts("DriverKit task identity startup rejection passed");
        return 0;
    }
    assert(!IS_ERR(task));
    wake_up_process(task);
    /* Let the worker finish before stop(), which otherwise may skip entry. */
    for (int i = 0; i < 1000; i++) {
        if (fail_create || atomic_load(&set_calls) >= (fail_set_call == 2 ? 2 : 1))
            break;
        usleep(100);
    }
    if (fail_set_call == 2)
        for (int i = 0; i < 1000 && atomic_load(&set_calls) < 2; i++) usleep(100);
    assert(kthread_stop(task) == -EAGAIN);
    if (fail_set_call == 2) {
        assert(atomic_load(&entry_calls) == 1);
        assert(failed_restore_task == task);
        /* This is the pointer that a reused dispatch worker would read. */
        assert(!strcmp(failed_restore_task->comm, "tls-failure"));
        assert(failed_restore_task->group_leader == failed_restore_task);
    } else {
        assert(atomic_load(&entry_calls) == 0);
    }
    puts("DriverKit task identity failure containment passed");
    return 0;
}
