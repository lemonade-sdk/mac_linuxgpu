#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
flags=(-w -std=gnu11 -g -O1 -fsanitize=address,undefined
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers)
clang "${flags[@]}" -Dpthread_cond_broadcast=ww_test_broadcast \
  -c linuxu/src/sync/ww_mutex.c -o "$test_dir/ww_mutex.o"
clang "${flags[@]}" linuxu/tests/test_ww_mutex.c \
  "$test_dir/ww_mutex.o" linuxu/src/sync.c \
  linuxu/src/shims/task.c linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -lpthread -o "$test_dir/test_ww_mutex"
"$test_dir/test_ww_mutex"
