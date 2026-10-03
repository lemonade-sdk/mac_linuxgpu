#ifndef LINUXU_TEST_DEXT_HEAP_BACKEND_H
#define LINUXU_TEST_DEXT_HEAP_BACKEND_H
#include <stddef.h>

/* Checked IOMalloc/IOFree backend for offline production-heap tests. */
size_t dext_heap_test_live_allocations(void);
size_t dext_heap_test_live_bytes(void);
void dext_heap_test_fail_after(long successful_allocations);
char *dext_heap_test_foreign_strdup(const char *source);
void dext_heap_test_enable_abort_trace(void);
#endif
