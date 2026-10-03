#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -w -std=gnu11 -g -O1 -fsanitize=address,undefined \
  -fno-sanitize-recover=all -ffunction-sections -fdata-sections \
  -Ilinuxu/headers linuxu/tests/test_drm_exec_lifetime.c \
  linuxu/src/drm/drm_exec.c linuxu/src/sync/ww_mutex.c \
  linuxu/src/sync.c linuxu/src/refcount.c linuxu/src/bug.c \
  linuxu/src/shims/task.c linuxu/src/shims/kthread.c -Wl,-dead_strip -lpthread -o "$test_dir/test_drm_exec_lifetime"
"$test_dir/test_drm_exec_lifetime"
