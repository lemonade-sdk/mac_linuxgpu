#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <rt/klog.h>

extern void klog_set_level(int);
extern void dev_printk_impl(const char *, const char *, const char *, ...);

#ifdef LINUXU_DEXT_DK
int IOLog(const char *format, ...)
{
    // The platform sink may reenter diagnostics: the ring lock must already
    // be released.
    uint64_t cursor = UINT64_MAX, end;
    assert(!klog_read(&cursor, NULL, 0, &end) && cursor == end);
    va_list arguments;
    va_start(arguments, format);
    int n = vfprintf(stderr, format, arguments);
    va_end(arguments);
    return n;
}
#endif

static uint64_t tail(void)
{
    uint64_t cursor = UINT64_MAX, end;
    assert(!klog_read(&cursor, NULL, 0, &end));
    assert(cursor == end);
    return end;
}

static int bounded_format(char *buf, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vscnprintf(buf, size, format, args);
    va_end(args);
    return written;
}

static void log_arguments(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintk(format, args);
    va_end(args);
}

static void variadic_contract(void)
{
    char buffer[5];
    assert(bounded_format(buffer, sizeof(buffer), "%s-%d", "abcdef", 42) == 4);
    assert(!strcmp(buffer, "abcd"));
    assert(!bounded_format(NULL, 0, "%d", 42));
    uint64_t cursor = tail(), end;
    log_arguments(KERN_WARNING "args=%s/%d/%llu", "checked", -7, 1234567890123ull);
    char text[128] = {0};
    assert(klog_read(&cursor, text, sizeof(text) - 1, &end));
    assert(!strcmp(text, "<4> args=checked/-7/1234567890123\n"));
}

static void ratelimit_contract(void)
{
    struct ratelimit_state state;
    ratelimit_state_init(&state, HZ, 3);
    for (unsigned i = 0; i < 3; ++i) assert(__ratelimit(&state));
    assert(!__ratelimit(&state) && atomic_read(&state.missed) == 1);
    ratelimit_state_reset_interval(&state, HZ);
    assert(__ratelimit(&state));
    state.begin -= 1001;
    for (unsigned i = 0; i < 3; ++i) assert(__ratelimit(&state));
    assert(!__ratelimit(&state));
    ratelimit_state_init(&state, 0, 0);
    for (unsigned i = 0; i < 10; ++i) assert(__ratelimit(&state));
    ratelimit_state_init(&state, HZ, 0);
    assert(!__ratelimit(&state));
}

static void basic(void)
{
    assert(!tail());
    printk(KERN_ERR "probe failed r=%d fw=%s", -19, "gc12");
    char text[128] = {0};
    uint64_t cursor = 0, end;
    size_t count = klog_read(&cursor, text, sizeof(text) - 1, &end);
    assert(count == strlen("<3> probe failed r=-19 fw=gc12\n"));
    assert(!strcmp(text, "<3> probe failed r=-19 fw=gc12\n"));
    assert(cursor == end && end == count);
    fflush(stderr);
    char emitted[128] = {0};
    assert(pread(fileno(stderr), emitted, sizeof(emitted) - 1, 0) > 0);
#ifdef LINUXU_DEXT_DK
    /* An error is an event: the unified log gets the retained record. */
    assert(!strcmp(emitted, "mac.linuxgpu: EVENT <3> probe failed r=-19 fw=gc12\n"));
#else
    assert(!strcmp(emitted, "<3> probe failed r=-19 fw=gc12\n"));
#endif
    klog_set_level(3);
    printk(KERN_INFO "filtered");
    assert(tail() == end);
    klog_set_level(6);
    dev_printk_impl(KERN_ERR, "gpu0", "error=%x\n", 0x55);
    memset(text, 0, sizeof(text));
    assert(klog_read(&cursor, text, sizeof(text) - 1, &end));
    assert(!strcmp(text, "<3> gpu0: error=55\n"));
    uint64_t invalid = 17, unchanged = 19;
    assert(!klog_read(NULL, text, 1, &unchanged) && unchanged == 19);
    assert(!klog_read(&invalid, NULL, 1, &unchanged));
    assert(invalid == 17 && unchanged == 19);
}

static void wrap(void)
{
    char oracle[LINUXU_KLOG_CAPACITY * 4], snapshot[LINUXU_KLOG_CAPACITY];
    size_t expected = 0;
    uint64_t start = tail();
    for (unsigned i = 0; i < 512; i++) {
        const char payload[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        int count = snprintf(oracle + expected, sizeof(oracle) - expected,
            "<6> R%06u:%s\n", i, payload);
        assert(count > 0 && (size_t)count < sizeof(oracle) - expected);
        expected += count;
        printk(KERN_INFO "R%06u:%s", i, payload);
    }
    assert(expected > sizeof(snapshot));
    uint64_t cursor = 0, end;
    size_t count = klog_read(&cursor, snapshot, sizeof(snapshot), &end);
    assert(count == sizeof(snapshot) && end == start + expected && cursor == end);
    assert(cursor - count > 0); // explicit stale-cursor / dropped-byte evidence
    assert(!memcmp(snapshot, oracle + expected - count, count));

    cursor = 0;
    assert(!klog_read(&cursor, NULL, 0, &end));
    assert(cursor == end - LINUXU_KLOG_CAPACITY);
    size_t copied = 0;
    while (copied < sizeof(snapshot)) {
        size_t size = sizeof(snapshot) - copied;
        if (size > 17) size = 17;
        count = klog_read(&cursor, snapshot + copied, size, &end);
        assert(count == size);
        copied += count;
    }
    assert(cursor == end && !memcmp(snapshot, oracle + expected - copied, copied));
    cursor = UINT64_MAX;
    assert(!klog_read(&cursor, snapshot, 1, &end) && cursor == end);
}

static void truncation(void)
{
    char oversized[8192], record[LINUXU_KLOG_MESSAGE_CAPACITY];
    memset(oversized, 'X', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    uint64_t cursor = tail(), end;
    fflush(stderr);
    struct stat before, after;
    assert(!fstat(fileno(stderr), &before));
    printk(KERN_WARNING "%s", oversized);
    fflush(stderr);
    assert(!fstat(fileno(stderr), &after));
#ifdef LINUXU_DEXT_DK
    /* A warning is not an event: retained only. */
    assert(after.st_size == before.st_size);
#else
    assert(after.st_size - before.st_size == sizeof(oversized) + 4);
#endif
    assert(klog_read(&cursor, record, sizeof(record), &end) == sizeof(record));
    assert(cursor == end && !memcmp(record, "<4> XXXX", 8));
    const char marker[] = "... [truncated]\n";
    assert(!memcmp(record + sizeof(record) - (sizeof(marker) - 1), marker, sizeof(marker) - 1));
}

static void retained_only(void)
{
    char message[LINUXU_KLOG_CAPACITY + 17], snapshot[LINUXU_KLOG_CAPACITY];
    for (size_t i = 0; i < sizeof(message); ++i) message[i] = (char)(i % 251);
    uint64_t cursor = tail(), initial = cursor, end;
    struct stat before, after;
    fflush(stderr);
    assert(!fstat(fileno(stderr), &before));
    klog_set_level(0); /* lifecycle records survive printk filtering */
    klog_write(NULL, 1);
    klog_write(message, 0);
    assert(tail() == initial);
    klog_write(message, sizeof(message));
    assert(klog_read(&cursor, snapshot, sizeof(snapshot), &end) == sizeof(snapshot));
    assert(cursor == end && end - initial == sizeof(snapshot));
    assert(!memcmp(snapshot, message + sizeof(message) - sizeof(snapshot), sizeof(snapshot)));
    fflush(stderr);
    assert(!fstat(fileno(stderr), &after) && after.st_size == before.st_size);
    klog_set_level(6);
}

#define WRITERS 4u
#define MESSAGES 1200u
static unsigned completed;
static const char payload[] = "abcdefghijklmnopqrstuvwxyz0123456789";
static void *writer(void *opaque)
{
    unsigned id = (unsigned)(uintptr_t)opaque;
    for (unsigned i = 0; i < MESSAGES; i++)
        printk(KERN_INFO "T%u:%04u:%s", id, i, payload);
    __atomic_add_fetch(&completed, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void concurrent(void)
{
    pthread_t threads[WRITERS];
    unsigned seen = 0, latest[WRITERS] = {0}, have[WRITERS] = {0};
    uint64_t cursor = tail(), initial = cursor, end = cursor;
    for (unsigned i = 0; i < WRITERS; i++)
        assert(!pthread_create(&threads[i], NULL, writer, (void *)(uintptr_t)i));
    for (;;) {
        char snapshot[LINUXU_KLOG_CAPACITY];
        uint64_t requested = cursor;
        size_t count = klog_read(&cursor, snapshot, sizeof(snapshot), &end);
        assert(cursor <= end && end >= requested);
        char *line = snapshot, *limit = snapshot + count;
        if (count && cursor - count != requested) {
            // A stale cursor can start in the middle of an overwritten record.
            line = memchr(line, '\n', count);
            assert(line); ++line;
        }
        while (line < limit) {
            char *newline = memchr(line, '\n', (size_t)(limit - line));
            assert(newline); // published snapshots end at complete records
            char actual[128], expected[128];
            size_t length = (size_t)(newline + 1 - line);
            assert(length < sizeof(actual));
            memcpy(actual, line, length); actual[length] = 0;
            unsigned id, sequence;
            assert(sscanf(actual, "<6> T%u:%u:", &id, &sequence) == 2);
            assert(id < WRITERS && sequence < MESSAGES);
            assert(!have[id] || sequence > latest[id]);
            have[id] = 1; latest[id] = sequence;
            snprintf(expected, sizeof(expected), "<6> T%u:%04u:%s\n", id, sequence, payload);
            assert(!strcmp(actual, expected));
            ++seen; line = newline + 1;
        }
        if (!count && __atomic_load_n(&completed, __ATOMIC_ACQUIRE) == WRITERS) break;
    }
    for (unsigned i = 0; i < WRITERS; i++) assert(!pthread_join(threads[i], NULL));
    char sample[128];
    int length = snprintf(sample, sizeof(sample), "<6> T0:0000:%s\n", payload);
    assert(seen && tail() - initial == (uint64_t)WRITERS * MESSAGES * length);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    alarm(20);
    assert(freopen(argv[1], "w+", stderr));
    basic(); variadic_contract(); ratelimit_contract(); wrap(); truncation(); retained_only(); concurrent();
#ifdef LINUXU_DEXT_DK
    puts("PASS DriverKit klog: sink preservation, wrap, cursor clamping, truncation, concurrent snapshots and sink reentry");
#else
    puts("PASS host klog: sink preservation, wrap, cursor clamping, truncation and concurrent snapshots");
#endif
}
