/* Exercise the public headers, including the rwlock aliases seen by DRM. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/rwlock.h>
#include <linux/seqlock.h>
#include <linux/completion.h>
#include <linux/semaphore.h>
#include <linux/cleanup.h>

static unsigned guards_entered, guards_exited;
static void enter_guard(int *held)
{ assert(!*held); *held = 1; guards_entered++; }
static void leave_guard(int *held)
{ assert(*held == 1); *held = 0; guards_exited++; }
DEFINE_GUARD(audit_guard, int *, enter_guard(_T), leave_guard(_T))

static void early_guard_return(int *held)
{
    guard(audit_guard)(held);
    assert(*held);
    return;
}

static void cleanup_contracts(void)
{
    int held = 0;
    early_guard_return(&held);
    assert(!held && guards_entered == guards_exited);
    scoped_guard(audit_guard, &held) { assert(held); break; }
    assert(!held && guards_entered == guards_exited);
    unsigned visits = 0;
    scoped_guard(audit_guard, &held) { visits++; continue; }
    assert(visits == 1 && !held && guards_entered == guards_exited);
    { guard(audit_guard)(&held); goto done; }
done:
    assert(!held && guards_entered == guards_exited);
    int one = 1, two = 2;
    int *owners[2] = {&one, &two};
    unsigned slot = 0;
    int *taken = no_free_ptr(owners[slot++]);
    assert(slot == 1 && taken == &one && !owners[0] && owners[1] == &two);
    struct mutex lock;
    mutex_init(&lock);
    { guard(mutex)(&lock); assert(mutex_is_locked(&lock)); }
    assert(!mutex_is_locked(&lock));
    mutex_destroy(&lock);
}

void msleep(unsigned int ms) { usleep(ms * 1000); }
void usleep_range(unsigned long min, unsigned long max)
{ (void)max; usleep((useconds_t)min); }

static DEFINE_SEQLOCK(snapshot_lock);
static unsigned payload_a, payload_b;
static atomic_int writer_done, reader_started, reader_done;
static void *snapshot_writer(void *unused)
{
    (void)unused;
    for (unsigned i = 1; i <= 100000; i++) {
        write_seqlock(&snapshot_lock);
        __atomic_store_n(&payload_a, i, __ATOMIC_RELAXED);
        __atomic_store_n(&payload_b, ~i, __ATOMIC_RELAXED);
        write_sequnlock(&snapshot_lock);
    }
    atomic_store(&writer_done, 1);
    return NULL;
}
static void *snapshot_reader(void *unused)
{
    (void)unused;
    atomic_store(&reader_started, 1);
    unsigned a, b, sequence;
    do {
        do {
            sequence = read_seqbegin(&snapshot_lock);
            a = __atomic_load_n(&payload_a, __ATOMIC_RELAXED);
            b = __atomic_load_n(&payload_b, __ATOMIC_RELAXED);
        } while (read_seqretry(&snapshot_lock, sequence));
        assert(b == ~a);
    } while (!atomic_load(&writer_done));
    atomic_store(&reader_done, 1);
    return NULL;
}
int main(void)
{
    alarm(20);
    cleanup_contracts();
    DECLARE_COMPLETION(event);
    DEFINE_SEMAPHORE(sem, 1);
    complete(&event); assert(try_wait_for_completion(&event));
    assert(!down_trylock(&sem)); up(&sem);
    struct list_head marker;
    INIT_LIST_HEAD(&marker);
    list_add(&marker, &event.wait);
    complete_all(&event);
    reinit_completion(&event);
    assert(!completion_done(&event) && event.wait.next == &marker);
    list_del(&marker);
    DEFINE_SPINLOCK(spin);
    DEFINE_RAW_SPINLOCK(raw);
    DEFINE_RWLOCK(lock);
    unsigned long flags = 99;
    spin_lock_irqsave(&spin, flags);
    assert(flags == 0 && spin_is_locked(&spin));
    spin_unlock_irqrestore(&spin, flags);
    flags = 99;
    raw_spin_lock_irqsave(&raw, flags);
    assert(flags == 0 && raw_spin_is_locked(&raw));
    raw_spin_unlock_irqrestore(&raw, flags);
    assert(write_trylock(&lock));
    assert(rwlock_is_write_locked(&lock));
    assert(!write_trylock(&lock) && !read_trylock(&lock));
    write_unlock(&lock);
    assert(read_trylock(&lock));
    assert(rwlock_is_read_locked(&lock));
    assert(read_trylock(&lock) && !write_trylock(&lock));
    read_unlock(&lock); read_unlock(&lock);
    assert(!rwlock_is_locked(&lock));
    write_lock_irqsave(&lock, flags);
    assert(flags == 0 && !write_trylock(&lock));
    write_unlock_irqrestore(&lock, flags);
    read_lock_irq(&lock); assert(!write_trylock(&lock)); read_unlock_irq(&lock);
    write_lock_irq(&lock); assert(!read_trylock(&lock)); write_unlock_irq(&lock);
    read_lock_irqsave(&lock, flags); read_unlock_irqrestore(&lock, flags);

    pthread_t reader, writer;
    write_seqlock(&snapshot_lock);
    payload_a = 0; payload_b = ~0u;
    assert(pthread_create(&reader, NULL, snapshot_reader, NULL) == 0);
    while (!atomic_load(&reader_started)) usleep(100);
    usleep(1000);
    assert(!atomic_load(&reader_done));
    write_sequnlock(&snapshot_lock);
    assert(!(snapshot_lock.s.seqcount & 1));
    assert(pthread_create(&writer, NULL, snapshot_writer, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(snapshot_lock.s.seqcount == 200002);
    puts("rwlock header exclusion and concurrent seqlock snapshots passed");
    return 0;
}
