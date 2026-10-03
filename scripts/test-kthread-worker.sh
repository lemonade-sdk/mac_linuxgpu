#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_kthread_worker.c linuxu/src/shims/task.c linuxu/src/bug.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_kthread_worker"
"$test_dir/test_kthread_worker"
