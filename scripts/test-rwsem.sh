#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_rwsem.c linuxu/src/rwsem.c linuxu/src/sync.c linuxu/src/shims/task.c linuxu/src/shims/kthread.c linuxu/src/bug.c -Wl,-dead_strip -lpthread \
  -o "$test_dir/test_rwsem"
"$test_dir/test_rwsem"
