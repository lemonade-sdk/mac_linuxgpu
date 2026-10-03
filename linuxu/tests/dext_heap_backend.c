#include "dext_heap_backend.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static size_t allocations, bytes;
static long fail_after = -1;

void dext_heap_test_fail_after(long count)
{
	pthread_mutex_lock(&lock);
	fail_after = count;
	pthread_mutex_unlock(&lock);
}

size_t dext_heap_test_live_allocations(void)
{
	pthread_mutex_lock(&lock);
	size_t result = allocations;
	pthread_mutex_unlock(&lock);
	return result;
}

size_t dext_heap_test_live_bytes(void)
{
	pthread_mutex_lock(&lock);
	size_t result = bytes;
	pthread_mutex_unlock(&lock);
	return result;
}

void *IOMalloc(size_t size)
{
	pthread_mutex_lock(&lock);
	if (fail_after == 0) {
		pthread_mutex_unlock(&lock);
		return NULL;
	}
	if (fail_after > 0)
		fail_after--;
	pthread_mutex_unlock(&lock);
	assert(size <= SIZE_MAX - sizeof(size_t) - 1);
	unsigned char *raw = malloc(size + sizeof(size_t) + 1);
	if (!raw)
		return NULL;
	memcpy(raw, &size, sizeof(size));
	pthread_mutex_lock(&lock);
	allocations++;
	bytes += size;
	pthread_mutex_unlock(&lock);
	/* Exercise alignment adjustment and the exact original address/length. */
	return raw + sizeof(size_t) + 1;
}

void IOFree(void *pointer, size_t size)
{
	assert(pointer);
	unsigned char *raw = (unsigned char *)pointer - sizeof(size_t) - 1;
	size_t expected;
	memcpy(&expected, raw, sizeof(expected));
	assert(expected == size);
	pthread_mutex_lock(&lock);
	assert(allocations && bytes >= size);
	allocations--;
	bytes -= size;
	pthread_mutex_unlock(&lock);
	free(raw);
}

char *dext_heap_test_foreign_strdup(const char *source)
{
	/* Negative control: an imported libc allocator has no dext header. */
	return strdup(source);
}

/* Keep Darwin's signal/execinfo declarations out of Linux driver headers. */
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
static void dext_heap_test_abort_trace(int signal_number)
{
	void *frames[40];
	int count = backtrace(frames, 40);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	_exit(128 + signal_number);
}
void dext_heap_test_enable_abort_trace(void)
{
	signal(SIGABRT, dext_heap_test_abort_trace);
}
