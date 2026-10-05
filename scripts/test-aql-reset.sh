#!/usr/bin/env bash
# The driver's AQL queues across a device reset (linuxu/tests/test_aql_reset.cpp):
# the real dext_aql.mm against a mock MES, under ASan/UBSan and under TSan.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
# DriverKit's IOLib, as much of it as dext_aql.mm uses.
mkdir -p "$test_dir/DriverKit"
cat > "$test_dir/DriverKit/IOLib.h" <<'HEADER'
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
typedef struct IOLock { pthread_mutex_t m; } IOLock;
static inline IOLock *IOLockAlloc(void)
{ IOLock *l = (IOLock *)calloc(1, sizeof(IOLock)); if (l) pthread_mutex_init(&l->m, NULL); return l; }
static inline void IOLockLock(IOLock *l) { pthread_mutex_lock(&l->m); }
static inline void IOLockUnlock(IOLock *l) { pthread_mutex_unlock(&l->m); }
static inline void *IOMallocZero(size_t size) { return calloc(1, size); }
static inline void IOFree(void *p, size_t size) { (void)size; free(p); }
static inline void IOSleep(unsigned ms) { usleep(ms * 1000); }
HEADER
common=(-std=c++17 -g -O1 -w -I"$test_dir" -Ilinuxu/headers -Idext/sources)
clang++ "${common[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
  linuxu/tests/test_aql_reset.cpp -o "$test_dir/test_aql_reset"
"$test_dir/test_aql_reset"
clang++ "${common[@]}" -fsanitize=thread linuxu/tests/test_aql_reset.cpp -o "$test_dir/test_aql_reset_tsan"
TSAN_OPTIONS=halt_on_error=1 "$test_dir/test_aql_reset_tsan" > /dev/null
echo "PASS AQL reset under TSan: no data race between the reset hooks and the owner's calls"
