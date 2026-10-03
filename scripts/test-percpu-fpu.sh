#!/usr/bin/env bash
# Per-task per-CPU copies and kernel FPU sections, under ASan/UBSan.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -D__KERNEL__ -include linux/autoconf.h \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -ffunction-sections -fdata-sections -Ilinuxu/headers \
  linuxu/tests/test_percpu_fpu.c linuxu/src/fpu.c linuxu/src/shims/task.c \
  linuxu/src/shims/kthread.c linuxu/src/sync.c linuxu/src/bug.c \
  -Wl,-dead_strip -lpthread -o "$test_dir/test_percpu_fpu"
"$test_dir/test_percpu_fpu"
