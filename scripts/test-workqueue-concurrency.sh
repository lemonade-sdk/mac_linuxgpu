#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all
  -ffunction-sections -fdata-sections -Ilinuxu/headers)
support=(linuxu/src/sync.c linuxu/src/delay.c linuxu/src/shims/task.c
  linuxu/src/shims/kthread.c linuxu/src/bug.c)

# Host pthreads.
clang "${flags[@]}" linuxu/tests/test_workqueue_concurrency.c linuxu/src/work.c \
  "${support[@]}" -Wl,-dead_strip -lpthread -o "$test_dir/host"
"$test_dir/host"

# The dext thread backend: work.c's threads become dext_threads.c tasks over
# the mocked IODispatchQueue/TLS bridge from test_dext_threads.c.
rename=(-Dpthread_create=dext_test_pthread_create -Dpthread_join=dext_test_pthread_join
  -Dpthread_self=dext_test_pthread_self -Dpthread_equal=dext_test_pthread_equal)
clang "${flags[@]}" "${rename[@]}" -c linuxu/src/work.c -o "$test_dir/work.o"
clang "${flags[@]}" -DLINUXU_TEST_DEXT_THREADS -c linuxu/src/shims/dext_threads.c \
  -o "$test_dir/threads.o"
clang "${flags[@]}" -Dmain=dext_threads_fixture_unused_main \
  -c linuxu/tests/test_dext_threads.c -o "$test_dir/dispatch.o"
clang "${flags[@]}" linuxu/tests/test_workqueue_concurrency.c "$test_dir/work.o" \
  "$test_dir/threads.o" "$test_dir/dispatch.o" "${support[@]}" \
  -Wl,-dead_strip -lpthread -o "$test_dir/dext"
"$test_dir/dext"
